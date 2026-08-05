/**
 * @file app_pwm_input_config.h
 * @brief 通用 PWM 输入编译期配置。
 *
 * 配置职责：选择 DMA 自动量程或 Legacy 中断引擎，定义 1~200 kHz 软件范围、三档计数时钟、数字滤波和 DMA 缓冲参数。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_PWM_INPUT_CONFIG_H
#define APP_PWM_INPUT_CONFIG_H

/* Engine selection. DMA autorange is the Step23 default. The legacy Step22
 * interrupt engine remains available as a one-line rollback. */
#define APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY  0U
#define APP_PWM_INPUT_ENGINE_DMA_AUTORANGE     1U

#ifndef APP_PWM_INPUT_ENGINE_MODE
#define APP_PWM_INPUT_ENGINE_MODE APP_PWM_INPUT_ENGINE_DMA_AUTORANGE
#endif

#if (APP_PWM_INPUT_ENGINE_MODE != APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY) && \
    (APP_PWM_INPUT_ENGINE_MODE != APP_PWM_INPUT_ENGINE_DMA_AUTORANGE)
#error "Invalid APP_PWM_INPUT_ENGINE_MODE"
#endif

/* Public software range. The W2 object 0x03 layout is unchanged. */
#define APP_PWM_INPUT_MIN_FREQ_HZ              1U
#define APP_PWM_INPUT_MAX_FREQ_HZ              200000U
#define APP_PWM_INPUT_TIMEOUT_MS               2500U

/* The current external polarity/duty semantics are intentionally preserved. */
#define APP_PWM_INPUT_INVERTED                 0U
#define APP_PWM_INPUT_IC_FILTER                2U

/* Step22 legacy engine: TIM1 input clock 168 MHz, PSC=7 -> 21 MHz. */
#define APP_PWM_INPUT_TIM_PSC                  7U
#define APP_PWM_INPUT_COUNTER_CLOCK_HZ         21000000U
#define APP_PWM_INPUT_LEGACY_MAX_FREQ_HZ       20000U

/* Step23 DMA PWM-input autorange profiles. TIM1 is a 16-bit timer. */
#define APP_PWM_INPUT_DMA_FAST_PSC             7U
#define APP_PWM_INPUT_DMA_FAST_CLOCK_HZ        21000000U
#define APP_PWM_INPUT_DMA_MEDIUM_PSC           167U
#define APP_PWM_INPUT_DMA_MEDIUM_CLOCK_HZ      1000000U
#define APP_PWM_INPUT_DMA_SLOW_PSC             3359U
#define APP_PWM_INPUT_DMA_SLOW_CLOCK_HZ        50000U

/* Up-range thresholds are expressed in the active profile's timer ticks.
 * Slow: period < 200 ticks -> >250 Hz, move to medium.
 * Medium: period < 200 ticks -> >5 kHz, move to fast. */
#define APP_PWM_INPUT_DMA_SLOW_UP_TICKS         200U
#define APP_PWM_INPUT_DMA_MEDIUM_UP_TICKS       200U

/* DMA2 Stream6 Channel0 is mapped to TIM1_CH1 on STM32F407. A CC1 event
 * triggers a two-register timer DMA burst: CCR1 period + CCR2 external-high
 * time. No DMA interrupt is used; the main loop reads the newest samples. */
#define APP_PWM_INPUT_DMA_BUFFER_PAIRS          128U
#define APP_PWM_INPUT_DMA_MEDIAN_SAMPLES        5U
#define APP_PWM_INPUT_DMA_DISABLE_TIMEOUT_LOOPS 100000U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
