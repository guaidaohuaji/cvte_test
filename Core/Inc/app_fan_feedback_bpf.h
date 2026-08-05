/**
 * @file app_fan_feedback_bpf.h
 * @brief 风机 FG 带通滤波与过零测速（公共接口头文件）。
 *
 * 模块职责：对 10 kS/s ADC 样本执行定点双二阶带通滤波，使用迟滞过零、周期连续性和 5 周期中位数估算 30~82 Hz FG。
 * 数据输入：逐个 PA0 ADC 原始样本及采样块边界。
 * 数据输出：滤波状态、过零周期历史、频率、有效性和拒绝计数。
 * 执行上下文：由 app_fan_feedback_adc.c 在处理 ADC 块时逐样本调用；无动态内存。
 * 阅读重点：先看 biquad 状态和系数，再看 process_crossing()/accept_period()，最后看 warm-up、timeout 和 block 统计。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_FAN_FEEDBACK_BPF_H
#define APP_FAN_FEEDBACK_BPF_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The historical SHADOW names are retained to avoid broad project churn.
 * In stage 3 the filter/tachometer may be selected as the official feedback
 * source by app_fan_feedback_adc.c, while the legacy detector remains alive
 * for diagnostics and optional fallback.
 */
#ifndef APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
#define APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE  1U
#endif

#ifndef APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
#define APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE  1U
#endif

#ifndef APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ
#define APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ  10000U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_LOW_HZ
#define APP_FAN_FEEDBACK_BPF_LOW_HZ              20U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_HIGH_HZ
#define APP_FAN_FEEDBACK_BPF_HIGH_HZ            100U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_WARMUP_MS
#define APP_FAN_FEEDBACK_BPF_WARMUP_MS           150U
#endif

/* Filtered tachometer tuning. */
#ifndef APP_FAN_FEEDBACK_BPF_TACH_MIN_HZ
#define APP_FAN_FEEDBACK_BPF_TACH_MIN_HZ          30U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_MAX_HZ
#define APP_FAN_FEEDBACK_BPF_TACH_MAX_HZ          82U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_RPM_FACTOR
#define APP_FAN_FEEDBACK_BPF_TACH_RPM_FACTOR      30U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE
#define APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE     5U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS
#define APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS 3U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_PERIOD_TOLERANCE_PERCENT
#define APP_FAN_FEEDBACK_BPF_TACH_PERIOD_TOLERANCE_PERCENT 25U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_RESYNC_REJECTS
#define APP_FAN_FEEDBACK_BPF_TACH_RESYNC_REJECTS   2U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_TIMEOUT_MS
#define APP_FAN_FEEDBACK_BPF_TACH_TIMEOUT_MS      500U
#endif

/*
 * The hysteresis is used only to arm a crossing.  The timestamp itself is
 * taken from a linearly interpolated zero crossing, reducing amplitude-
 * dependent phase error.
 */
#ifndef APP_FAN_FEEDBACK_BPF_TACH_HYST_RMS_PERCENT
#define APP_FAN_FEEDBACK_BPF_TACH_HYST_RMS_PERCENT 20U
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_MIN_HYSTERESIS
#define APP_FAN_FEEDBACK_BPF_TACH_MIN_HYSTERESIS  2.0f
#endif
#ifndef APP_FAN_FEEDBACK_BPF_TACH_MIN_SIGNAL_RMS
#define APP_FAN_FEEDBACK_BPF_TACH_MIN_SIGNAL_RMS  6.0f
#endif

typedef struct
{
    uint8_t enabled;
    uint8_t warmed_up;
    uint16_t reserved;

    uint32_t processed_samples;
    uint32_t warmup_remaining_samples;

    float output_last;
    float block_min;
    float block_max;
    float block_peak_to_peak;
    float block_rms;
    uint32_t block_samples;

    uint32_t nonfinite_count;
    uint32_t reset_count;

    /* Filtered tachometer diagnostics. */
    uint8_t tach_shadow_enabled;
    uint8_t tach_signal_present;
    uint8_t tach_armed;
    uint8_t tach_valid;

    uint32_t tach_freq_millihz;
    uint32_t tach_rpm;
    uint32_t tach_period_samples;
    uint32_t tach_update_seq;

    float tach_hysteresis;
    float tach_rms_estimate;

    uint32_t tach_rising_crossings;
    uint32_t tach_accepted_periods;
    uint32_t tach_short_rejected;
    uint32_t tach_post_short_rejected;
    uint32_t tach_long_rejected;
    uint32_t tach_inconsistent_rejected;
    uint32_t tach_resync_count;
    uint32_t tach_timeout_count;
    uint32_t tach_period_history_count;
    uint32_t tach_last_crossing_age_samples;
} AppFanFeedbackBpfStats;

void AppFanFeedbackBpf_Init(void);
void AppFanFeedbackBpf_Reset(void);
void AppFanFeedbackBpf_BeginBlock(void);
void AppFanFeedbackBpf_ProcessSample(uint16_t sample);
void AppFanFeedbackBpf_EndBlock(void);
bool AppFanFeedbackBpf_GetStats(AppFanFeedbackBpfStats *stats);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
