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

#endif
