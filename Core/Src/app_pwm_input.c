/**
 * @file app_pwm_input.c
 * @brief TIM1 通用 PWM 输入测量（实现文件）。
 *
 * 模块职责：默认使用 PWM Input + DMA 自动量程测量周期和外部高电平时间，同时保留 Step22 双边沿中断实现作为编译期回退。
 * 数据输入：PE9/TIM1 输入波形；TIM1 捕获寄存器和 DMA2 循环缓冲。
 * 数据输出：频率 mHz、占空比 x100、状态、量程和诊断计数快照。
 * 执行上下文：DMA 默认路径不使用捕获中断，AppPwmInput_Process() 在主循环批量读取最新样本；旧引擎通过宏选择。
 * 阅读重点：先看 app_pwm_input_config.h 的引擎与三档时钟，再看 DMA 每对数据的含义，最后看自动升降量程和静态电平超时。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_pwm_input.h"

#include <stddef.h>

#include "app_pwm_input_config.h"
#include "tim.h"

#if APP_PWM_INPUT_ENGINE_MODE == APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY

typedef enum { EXPECT_RISING, EXPECT_FALLING } ExpectedEdge;

static volatile uint32_t overflow_count;
static volatile uint32_t capture_seq;
static volatile uint32_t raw_period_ticks;
static volatile uint32_t raw_high_ticks;
static volatile bool     raw_valid;
static volatile uint32_t raw_ovc;
static volatile uint32_t raw_last_cap_ms;
static volatile bool     raw_synced;

static ExpectedEdge expected_edge;
static bool     synchronized;
static bool     rise_valid;
static bool     fall_valid;
static uint64_t last_rise_ts;
static uint64_t last_fall_ts;
static uint32_t ovc_count;
static uint32_t last_cap_ms_hw;

static AppPwmInputSnapshot cached_snap;

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param period_ticks 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool legacy_check_freq_range(uint32_t period_ticks)
{
    uint64_t hz1000;

    if (period_ticks == 0U) return false;
    hz1000 = (uint64_t)APP_PWM_INPUT_COUNTER_CLOCK_HZ * 1000ULL /
             (uint64_t)period_ticks;
    return (hz1000 >= ((uint64_t)APP_PWM_INPUT_MIN_FREQ_HZ * 1000ULL)) &&
           (hz1000 <= ((uint64_t)APP_PWM_INPUT_LEGACY_MAX_FREQ_HZ * 1000ULL));
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwmInput_Init(void)
{
    __HAL_TIM_DISABLE(&htim1);
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);

    __HAL_TIM_SET_PRESCALER(&htim1, APP_PWM_INPUT_TIM_PSC);
    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    htim1.Instance->EGR = TIM_EGR_UG;

    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_CC1);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_CC1OF);

    overflow_count = 0U;
    synchronized   = false;
    rise_valid     = false;
    fall_valid     = false;
    last_rise_ts   = 0ULL;
    last_fall_ts   = 0ULL;
    ovc_count      = 0U;
    last_cap_ms_hw = 0U;

    raw_period_ticks = 0U;
    raw_high_ticks   = 0U;
    raw_ovc          = 0U;
    raw_last_cap_ms  = 0U;
    raw_valid        = false;
    raw_synced       = false;
    capture_seq      = 0U;

    expected_edge = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_SET)
                    ? EXPECT_FALLING : EXPECT_RISING;

    cached_snap = (AppPwmInputSnapshot){0};
    cached_snap.status = APP_PWM_IN_SEARCH;
    cached_snap.prescaler = APP_PWM_INPUT_TIM_PSC;
    cached_snap.engine_mode = APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY;
    cached_snap.range_profile = APP_PWM_INPUT_RANGE_FAST;

    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);

    if (HAL_TIM_IC_Start_IT(&htim1, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
        return false;
    }

    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param htim 触发回调的 HAL 定时器句柄。
 */
void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim)
{
    uint32_t sr;
    uint32_t ccr;
    uint32_t ovf;
    uint64_t ts;

    if (htim->Instance != TIM1) return;

    sr = htim->Instance->SR;

    if ((sr & TIM_SR_CC1OF) != 0U)
    {
        htim->Instance->SR = ~TIM_SR_CC1OF;
        ovc_count++;
        raw_ovc = ovc_count;
        synchronized = false;
        rise_valid   = false;
        fall_valid   = false;
        raw_valid     = false;
        raw_synced    = false;
        expected_edge = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_SET)
                        ? EXPECT_FALLING : EXPECT_RISING;
        return;
    }

    ccr = htim->Instance->CCR1;
    ovf = overflow_count;

    if (((sr & TIM_SR_UIF) != 0U) && (ccr < 0x8000U))
    {
        ovf++;
    }

    ts = ((uint64_t)ovf << 16) | (uint64_t)ccr;
    last_cap_ms_hw = HAL_GetTick();

    if (expected_edge == EXPECT_RISING)
    {
        if (rise_valid && fall_valid)
        {
            uint64_t period = ts - last_rise_ts;
            uint64_t high   = ts - last_fall_ts;

            if ((period > 0ULL) && (period < 0x100000000ULL) &&
                (high <= period) &&
                legacy_check_freq_range((uint32_t)period))
            {
                capture_seq++;
                raw_period_ticks = (uint32_t)period;
                raw_high_ticks   = (uint32_t)high;
                raw_ovc          = ovc_count;
                raw_last_cap_ms  = last_cap_ms_hw;
                raw_synced       = true;
                raw_valid        = true;
                synchronized     = true;
            }
            else
            {
                synchronized = false;
                raw_synced   = false;
            }
        }

        last_rise_ts = ts;
        rise_valid   = true;
        expected_edge = EXPECT_FALLING;
    }
    else
    {
        last_fall_ts = ts;
        fall_valid   = true;
        expected_edge = EXPECT_RISING;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param htim 触发回调的 HAL 定时器句柄。
 */
void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1) overflow_count++;
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppPwmInput_Process(void)
{
    uint32_t seq = capture_seq;
    bool valid = raw_valid;
    uint32_t period = raw_period_ticks;
    uint32_t high = raw_high_ticks;
    uint32_t ov = raw_ovc;
    uint32_t cap_ms = raw_last_cap_ms;
    bool synced = raw_synced;
    uint32_t now;
    uint32_t age;
    uint8_t pin_level;
    uint64_t freq_millihz;
    uint64_t ext_high;
    uint64_t duty_x100;

    if (seq != capture_seq) return;

    now = HAL_GetTick();
    age = (uint32_t)(now - cap_ms);
    pin_level = ((GPIOE->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

    if (!valid || !synced || age > APP_PWM_INPUT_TIMEOUT_MS)
    {
        if (age > APP_PWM_INPUT_TIMEOUT_MS)
        {
            if (pin_level != 0U)
            {
                cached_snap.status = APP_PWM_INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_LOW : APP_PWM_IN_STATIC_HIGH;
            }
            else
            {
                cached_snap.status = APP_PWM_INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_HIGH : APP_PWM_IN_STATIC_LOW;
            }
        }
        else
        {
            cached_snap.status = APP_PWM_IN_SEARCH;
        }
        cached_snap.raw_pin_level = pin_level;
        cached_snap.synced = 0U;
        cached_snap.age_ms = age;
        cached_snap.ovc_count = ovc_count;
        return;
    }

    freq_millihz = (uint64_t)APP_PWM_INPUT_COUNTER_CLOCK_HZ * 1000ULL /
                    (uint64_t)period;
#if APP_PWM_INPUT_INVERTED
    ext_high = (uint64_t)period - (uint64_t)high;
#else
    ext_high = high;
#endif
    duty_x100 = (ext_high * 10000ULL + (uint64_t)period / 2ULL) /
                (uint64_t)period;
    if (duty_x100 > 10000ULL) duty_x100 = 10000ULL;

    cached_snap.status = APP_PWM_IN_OK;
    cached_snap.freq_millihz = (uint32_t)freq_millihz;
    cached_snap.duty_x100 = (uint32_t)duty_x100;
    cached_snap.period_ticks = period;
    cached_snap.ext_high_ticks = (uint32_t)ext_high;
    cached_snap.age_ms = age;
    cached_snap.ovc_count = ov;
    cached_snap.prescaler = APP_PWM_INPUT_TIM_PSC;
    cached_snap.synced = 1U;
    cached_snap.raw_pin_level = pin_level;
}

#else /* APP_PWM_INPUT_ENGINE_DMA_AUTORANGE */

#define DMA_WORDS (APP_PWM_INPUT_DMA_BUFFER_PAIRS * 2U)
#define DMA_ERROR_FLAGS (DMA_HISR_TEIF6 | DMA_HISR_DMEIF6 | DMA_HISR_FEIF6)
#define DMA_CLEAR_FLAGS (DMA_HIFCR_CTCIF6 | DMA_HIFCR_CHTIF6 | \
                         DMA_HIFCR_CTEIF6 | DMA_HIFCR_CDMEIF6 | \
                         DMA_HIFCR_CFEIF6)

/* DMA burst 每次由 CC1 事件搬运两个 32 位槽：
 * period   = CCR1，一个完整输入周期的计数；
 * ext_high = CCR2，按当前已确认极性语义定义的外部高电平计数。
 * 两者必须来自同一次捕获，才能正确计算占空比。 */
typedef struct {
    uint32_t period;
    uint32_t ext_high;
} CapturePair;

static AppPwmInputSnapshot cached_snap;
static AppPwmInputRangeProfile active_profile;
static uint16_t last_producer_word;
static uint16_t valid_pair_count;
static uint32_t last_sample_ms;
static uint32_t dma_sample_count;
static uint32_t profile_switch_count;
static uint32_t dma_error_count;

#if defined(APP_PWM_INPUT_HOST_TEST)
static uint32_t host_dma_buffer[DMA_WORDS];
static uint16_t host_dma_producer;
static bool host_dma_wrapped;
static bool host_timer_overflow;
static bool host_dma_error;
#else
static volatile uint32_t dma_capture_buffer[DMA_WORDS] __attribute__((aligned(4)));
#endif

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t profile_prescaler(AppPwmInputRangeProfile profile)
{
    switch (profile)
    {
        case APP_PWM_INPUT_RANGE_MEDIUM: return APP_PWM_INPUT_DMA_MEDIUM_PSC;
        case APP_PWM_INPUT_RANGE_SLOW:   return APP_PWM_INPUT_DMA_SLOW_PSC;
        case APP_PWM_INPUT_RANGE_FAST:
        default:                         return APP_PWM_INPUT_DMA_FAST_PSC;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t profile_clock_hz(AppPwmInputRangeProfile profile)
{
    switch (profile)
    {
        case APP_PWM_INPUT_RANGE_MEDIUM: return APP_PWM_INPUT_DMA_MEDIUM_CLOCK_HZ;
        case APP_PWM_INPUT_RANGE_SLOW:   return APP_PWM_INPUT_DMA_SLOW_CLOCK_HZ;
        case APP_PWM_INPUT_RANGE_FAST:
        default:                         return APP_PWM_INPUT_DMA_FAST_CLOCK_HZ;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param period 见调用点；该参数只在本次调用期间有效。
 * @param clock_hz 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool pair_frequency_in_range(uint32_t period, uint32_t clock_hz)
{
    uint64_t freq_millihz;

    if (period == 0U) return false;
    freq_millihz = (uint64_t)clock_hz * 1000ULL / (uint64_t)period;
    return (freq_millihz >= ((uint64_t)APP_PWM_INPUT_MIN_FREQ_HZ * 1000ULL)) &&
           (freq_millihz <= ((uint64_t)APP_PWM_INPUT_MAX_FREQ_HZ * 1000ULL));
}

#if defined(APP_PWM_INPUT_HOST_TEST)
/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_start_profile(AppPwmInputRangeProfile profile)
{
    uint32_t i;
    (void)profile;
    for (i = 0U; i < DMA_WORDS; ++i) host_dma_buffer[i] = 0U;
    host_dma_producer = 0U;
    host_dma_wrapped = false;
    host_timer_overflow = false;
    host_dma_error = false;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t dma_hw_producer_word(void)
{
    return (uint16_t)(host_dma_producer & (uint16_t)~1U);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param index 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t dma_hw_word(uint16_t index)
{
    return host_dma_buffer[index % DMA_WORDS];
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_wrap_flag(void)
{
    bool value = host_dma_wrapped;
    host_dma_wrapped = false;
    return value;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_overflow_flag(void)
{
    bool value = host_timer_overflow;
    host_timer_overflow = false;
    return value;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_error_flag(void)
{
    bool value = host_dma_error;
    host_dma_error = false;
    return value;
}
#else
/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_disable_stream(void)
{
    uint32_t timeout = APP_PWM_INPUT_DMA_DISABLE_TIMEOUT_LOOPS;

    DMA2_Stream6->CR &= ~DMA_SxCR_EN;
    while (((DMA2_Stream6->CR & DMA_SxCR_EN) != 0U) && (timeout > 0U))
    {
        timeout--;
    }
    return timeout > 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_start_profile(AppPwmInputRangeProfile profile)
{
    uint32_t i;
    uint32_t ccmr1;

    TIM1->CR1 &= ~TIM_CR1_CEN;
    TIM1->DIER = 0U;
    TIM1->CCER = 0U;

    __HAL_RCC_DMA2_CLK_ENABLE();
    if (!dma_disable_stream()) return false;
    DMA2->HIFCR = DMA_CLEAR_FLAGS;

    for (i = 0U; i < DMA_WORDS; ++i) dma_capture_buffer[i] = 0U;

    DMA2_Stream6->PAR = (uint32_t)(uintptr_t)&TIM1->DMAR;
    DMA2_Stream6->M0AR = (uint32_t)(uintptr_t)dma_capture_buffer;
    DMA2_Stream6->NDTR = DMA_WORDS;
    DMA2_Stream6->FCR = 0U;
    DMA2_Stream6->CR = DMA_SxCR_PL_1 |
                       DMA_SxCR_MSIZE_1 |
                       DMA_SxCR_PSIZE_1 |
                       DMA_SxCR_MINC |
                       DMA_SxCR_CIRC;

    TIM1->PSC = profile_prescaler(profile);
    TIM1->ARR = 0xFFFFU;
    TIM1->CNT = 0U;
    TIM1->CR1 = TIM_CR1_URS;
    TIM1->CR2 = 0U;

    ccmr1 = TIM_CCMR1_CC1S_0 |
            TIM_CCMR1_CC2S_1 |
            ((uint32_t)APP_PWM_INPUT_IC_FILTER << TIM_CCMR1_IC1F_Pos) |
            ((uint32_t)APP_PWM_INPUT_IC_FILTER << TIM_CCMR1_IC2F_Pos);
    TIM1->CCMR1 = ccmr1;
    TIM1->CCMR2 = 0U;

    /* CH1: direct TI1, falling edge, period boundary/reset trigger.
     * CH2: indirect TI1, rising edge, preserving the existing external-high
     * duration semantics (physical low duration on PE9). */
    TIM1->CCER = TIM_CCER_CC1E | TIM_CCER_CC1P | TIM_CCER_CC2E;
    TIM1->SMCR = TIM_TS_TI1FP1 | TIM_SLAVEMODE_RESET;
    TIM1->DCR = TIM_DMABASE_CCR1 | TIM_DMABURSTLENGTH_2TRANSFERS;
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0U;

    DMA2_Stream6->CR |= DMA_SxCR_EN;
    TIM1->DIER = TIM_DIER_CC1DE;
    TIM1->CR1 |= TIM_CR1_CEN;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t dma_hw_producer_word(void)
{
    uint32_t remaining = DMA2_Stream6->NDTR;
    uint32_t producer;

    if (remaining > DMA_WORDS) remaining = DMA_WORDS;
    producer = DMA_WORDS - remaining;
    producer &= ~1UL;
    if (producer >= DMA_WORDS) producer = 0U;
    return (uint16_t)producer;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param index 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t dma_hw_word(uint16_t index)
{
    return dma_capture_buffer[index % DMA_WORDS];
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_wrap_flag(void)
{
    uint32_t flags = DMA2->HISR;
    bool wrapped = (flags & DMA_HISR_TCIF6) != 0U;
    DMA2->HIFCR = DMA_HIFCR_CTCIF6 | DMA_HIFCR_CHTIF6;
    return wrapped;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_overflow_flag(void)
{
    bool overflow = (TIM1->SR & TIM_SR_UIF) != 0U;
    TIM1->SR = (uint32_t)~TIM_SR_UIF;
    return overflow;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool dma_hw_take_error_flag(void)
{
    uint32_t flags = DMA2->HISR;
    bool error = (flags & DMA_ERROR_FLAGS) != 0U;
    if (error) DMA2->HIFCR = DMA_CLEAR_FLAGS;
    return error;
}
#endif

/**
 * @brief 清除内部临时状态，使后续流程重新同步。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void reset_cached_measurement(void)
{
    uint8_t pin_level = ((GPIOE->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

    cached_snap.status = APP_PWM_IN_SEARCH;
    cached_snap.freq_millihz = 0U;
    cached_snap.duty_x100 = 0U;
    cached_snap.period_ticks = 0U;
    cached_snap.ext_high_ticks = 0U;
    cached_snap.age_ms = 0U;
    cached_snap.ovc_count = dma_error_count;
    cached_snap.prescaler = profile_prescaler(active_profile);
    cached_snap.synced = 0U;
    cached_snap.raw_pin_level = pin_level;
    cached_snap.engine_mode = APP_PWM_INPUT_ENGINE_DMA_AUTORANGE;
    cached_snap.range_profile = (uint8_t)active_profile;
    cached_snap.dma_buffer_pairs = APP_PWM_INPUT_DMA_BUFFER_PAIRS;
    cached_snap.dma_sample_count = dma_sample_count;
    cached_snap.profile_switch_count = profile_switch_count;
    cached_snap.dma_error_count = dma_error_count;
}

/**
 * @brief 切换 FAST/MEDIUM/SLOW 量程，重置旧测量缓存并按新 PSC 重新启动 TIM1 和 DMA。
 * @param profile 输入捕获自动量程档位。
 * @param count_switch 是否累计本次量程切换。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool start_profile(AppPwmInputRangeProfile profile, bool count_switch)
{
    active_profile = profile;
    last_producer_word = 0U;
    valid_pair_count = 0U;
    last_sample_ms = HAL_GetTick();
    if (count_switch) profile_switch_count++;
    reset_cached_measurement();
    return dma_hw_start_profile(profile);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param index 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t wrapped_index(int32_t index)
{
    while (index < 0) index += (int32_t)DMA_WORDS;
    while (index >= (int32_t)DMA_WORDS) index -= (int32_t)DMA_WORDS;
    return (uint16_t)index;
}

/**
 * @brief 从 DMA 环形缓冲逆序收集最近的有效“周期/外部高时间”样本，并过滤零周期、脉宽大于周期和超出软件频率范围的数据。
 * @param producer 见调用点；该参数只在本次调用期间有效。
 * @param pairs 见调用点；该参数只在本次调用期间有效。
 * @param capacity 见调用点；该参数只在本次调用期间有效。
 * @param clock_hz 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t collect_recent_pairs(uint16_t producer,
                                     CapturePair *pairs,
                                     uint32_t capacity,
                                     uint32_t clock_hz)
{
    uint32_t available = valid_pair_count;
    uint32_t count = 0U;
    uint32_t i;

    if (available > capacity) available = capacity;
    for (i = 0U; i < available; ++i)
    {
        int32_t base = (int32_t)producer - (int32_t)(2U * (i + 1U));
        uint32_t period = dma_hw_word(wrapped_index(base));
        uint32_t ext_high = dma_hw_word(wrapped_index(base + 1));

        if ((period == 0U) || (ext_high > period)) continue;
        if (!pair_frequency_in_range(period, clock_hz)) continue;
        pairs[count].period = period;
        pairs[count].ext_high = ext_high;
        count++;
    }
    return count;
}

/**
 * @brief 按周期做小规模插入排序，后续取中间项作为抗单点毛刺的中位样本。
 * @param pairs 见调用点；该参数只在本次调用期间有效。
 * @param count 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void sort_pairs_by_period(CapturePair *pairs, uint32_t count)
{
    uint32_t i;

    for (i = 1U; i < count; ++i)
    {
        CapturePair key = pairs[i];
        uint32_t j = i;
        while ((j > 0U) && (pairs[j - 1U].period > key.period))
        {
            pairs[j] = pairs[j - 1U];
            j--;
        }
        pairs[j] = key;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param latest_period 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool should_up_range(uint32_t latest_period)
{
    if (active_profile == APP_PWM_INPUT_RANGE_SLOW)
    {
        return latest_period < APP_PWM_INPUT_DMA_SLOW_UP_TICKS;
    }
    if (active_profile == APP_PWM_INPUT_RANGE_MEDIUM)
    {
        return latest_period < APP_PWM_INPUT_DMA_MEDIUM_UP_TICKS;
    }
    return false;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppPwmInputRangeProfile next_faster_profile(void)
{
    if (active_profile == APP_PWM_INPUT_RANGE_SLOW)
        return APP_PWM_INPUT_RANGE_MEDIUM;
    return APP_PWM_INPUT_RANGE_FAST;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool can_down_range(void)
{
    return active_profile != APP_PWM_INPUT_RANGE_SLOW;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppPwmInputRangeProfile next_slower_profile(void)
{
    if (active_profile == APP_PWM_INPUT_RANGE_FAST)
        return APP_PWM_INPUT_RANGE_MEDIUM;
    return APP_PWM_INPUT_RANGE_SLOW;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwmInput_Init(void)
{
    cached_snap = (AppPwmInputSnapshot){0};
    dma_sample_count = 0U;
    profile_switch_count = 0U;
    dma_error_count = 0U;
    active_profile = APP_PWM_INPUT_RANGE_FAST;
    return start_profile(APP_PWM_INPUT_RANGE_FAST, false);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param htim 触发回调的 HAL 定时器句柄。
 */
void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim)
{
    (void)htim;
    /* Step23 DMA engine does not enable TIM1 capture interrupts. */
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param htim 触发回调的 HAL 定时器句柄。
 */
void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim)
{
    (void)htim;
    /* Step23 polls UIF in the main loop; TIM1 update IRQ remains disabled. */
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppPwmInput_Process(void)
{
    uint32_t now = HAL_GetTick();
    uint16_t producer = dma_hw_producer_word();
    bool wrapped = dma_hw_take_wrap_flag();
    bool overflow = dma_hw_take_overflow_flag();
    bool dma_error = dma_hw_take_error_flag();
    uint16_t delta_words;
    bool new_samples;
    uint32_t age;
    uint8_t pin_level;
    uint32_t clock_hz;
    uint16_t latest_base;
    uint32_t latest_period;
    CapturePair pairs[APP_PWM_INPUT_DMA_MEDIAN_SAMPLES];
    uint32_t pair_count;
    CapturePair selected;
    uint64_t freq_millihz;
    uint64_t duty_x100;

    if (dma_error)
    {
        dma_error_count++;
        (void)start_profile(active_profile, false);
        cached_snap.status = APP_PWM_IN_HW_ERROR;
        cached_snap.dma_error_count = dma_error_count;
        cached_snap.ovc_count = dma_error_count;
        return;
    }

    delta_words = (uint16_t)((producer + DMA_WORDS - last_producer_word) % DMA_WORDS);
    delta_words &= (uint16_t)~1U;
    new_samples = (delta_words >= 2U) || wrapped;

    if (new_samples)
    {
        uint32_t new_pairs = wrapped ? APP_PWM_INPUT_DMA_BUFFER_PAIRS :
                                      ((uint32_t)delta_words / 2U);
        if (new_pairs == 0U) new_pairs = APP_PWM_INPUT_DMA_BUFFER_PAIRS;
        if (new_pairs > APP_PWM_INPUT_DMA_BUFFER_PAIRS)
            new_pairs = APP_PWM_INPUT_DMA_BUFFER_PAIRS;

        dma_sample_count += new_pairs;
        if (wrapped)
            valid_pair_count = APP_PWM_INPUT_DMA_BUFFER_PAIRS;
        else
        {
            uint32_t total = (uint32_t)valid_pair_count + new_pairs;
            valid_pair_count = (uint16_t)((total > APP_PWM_INPUT_DMA_BUFFER_PAIRS) ?
                                APP_PWM_INPUT_DMA_BUFFER_PAIRS : total);
        }
        last_sample_ms = now;
        last_producer_word = producer;
    }

    /* 当前档位计数器溢出说明输入太慢：降低计数时钟，使更长周期
     * 能装入 16 位 TIM1 计数范围。 */
    if (overflow && can_down_range())
    {
        if (!start_profile(next_slower_profile(), true))
        {
            cached_snap.status = APP_PWM_IN_HW_ERROR;
            dma_error_count++;
        }
        return;
    }

    if (valid_pair_count > 0U)
    {
        latest_base = wrapped_index((int32_t)producer - 2);
        latest_period = dma_hw_word(latest_base);
        /* 周期计数过小意味着输入较快：提高计数时钟，增加周期和
         * 脉宽的计数分辨率。 */
        if (should_up_range(latest_period))
        {
            if (!start_profile(next_faster_profile(), true))
            {
                cached_snap.status = APP_PWM_IN_HW_ERROR;
                dma_error_count++;
            }
            return;
        }
    }

    age = (uint32_t)(now - last_sample_ms);
    pin_level = ((GPIOE->IDR & GPIO_PIN_9) != 0U) ? 1U : 0U;

    if ((valid_pair_count == 0U) || (age > APP_PWM_INPUT_TIMEOUT_MS))
    {
        if (age > APP_PWM_INPUT_TIMEOUT_MS)
        {
            if (pin_level != 0U)
            {
                cached_snap.status = APP_PWM_INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_LOW : APP_PWM_IN_STATIC_HIGH;
            }
            else
            {
                cached_snap.status = APP_PWM_INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_HIGH : APP_PWM_IN_STATIC_LOW;
            }
        }
        else
        {
            cached_snap.status = APP_PWM_IN_SEARCH;
        }
        cached_snap.synced = 0U;
        cached_snap.age_ms = age;
        cached_snap.raw_pin_level = pin_level;
        cached_snap.prescaler = profile_prescaler(active_profile);
        cached_snap.range_profile = (uint8_t)active_profile;
        cached_snap.dma_sample_count = dma_sample_count;
        cached_snap.profile_switch_count = profile_switch_count;
        cached_snap.dma_error_count = dma_error_count;
        cached_snap.ovc_count = dma_error_count;
        return;
    }

    clock_hz = profile_clock_hz(active_profile);
    pair_count = collect_recent_pairs(producer, pairs,
                                      APP_PWM_INPUT_DMA_MEDIAN_SAMPLES,
                                      clock_hz);
    if (pair_count == 0U)
    {
        latest_base = wrapped_index((int32_t)producer - 2);
        latest_period = dma_hw_word(latest_base);
        if ((active_profile == APP_PWM_INPUT_RANGE_FAST) &&
            (latest_period > 0U) &&
            (((uint64_t)clock_hz * 1000ULL / latest_period) >
             ((uint64_t)APP_PWM_INPUT_MAX_FREQ_HZ * 1000ULL)))
        {
            cached_snap.status = APP_PWM_IN_OUT_OF_RANGE;
        }
        else
        {
            cached_snap.status = APP_PWM_IN_SEARCH;
        }
        cached_snap.synced = 0U;
        cached_snap.age_ms = age;
        cached_snap.raw_pin_level = pin_level;
        return;
    }

    sort_pairs_by_period(pairs, pair_count);
    selected = pairs[pair_count / 2U];

    freq_millihz = (uint64_t)clock_hz * 1000ULL /
                    (uint64_t)selected.period;
    duty_x100 = ((uint64_t)selected.ext_high * 10000ULL +
                 (uint64_t)selected.period / 2ULL) /
                (uint64_t)selected.period;
    if (duty_x100 > 10000ULL) duty_x100 = 10000ULL;

    cached_snap.status = APP_PWM_IN_OK;
    cached_snap.freq_millihz = (uint32_t)freq_millihz;
    cached_snap.duty_x100 = (uint32_t)duty_x100;
    cached_snap.period_ticks = selected.period;
    cached_snap.ext_high_ticks = selected.ext_high;
    cached_snap.age_ms = age;
    cached_snap.ovc_count = dma_error_count;
    cached_snap.prescaler = profile_prescaler(active_profile);
    cached_snap.synced = 1U;
    cached_snap.raw_pin_level = pin_level;
    cached_snap.engine_mode = APP_PWM_INPUT_ENGINE_DMA_AUTORANGE;
    cached_snap.range_profile = (uint8_t)active_profile;
    cached_snap.dma_buffer_pairs = APP_PWM_INPUT_DMA_BUFFER_PAIRS;
    cached_snap.dma_sample_count = dma_sample_count;
    cached_snap.profile_switch_count = profile_switch_count;
    cached_snap.dma_error_count = dma_error_count;
}

#if defined(APP_PWM_INPUT_HOST_TEST)
/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param period_ticks 见调用点；该参数只在本次调用期间有效。
 * @param ext_high_ticks 见调用点；该参数只在本次调用期间有效。
 * @param tick_ms 见调用点；该参数只在本次调用期间有效。
 */
void AppPwmInput_TestFeedDmaPair(uint32_t period_ticks,
                                 uint32_t ext_high_ticks,
                                 uint32_t tick_ms)
{
    uint16_t next;
    (void)tick_ms;

    host_dma_buffer[host_dma_producer] = period_ticks;
    next = (uint16_t)((host_dma_producer + 1U) % DMA_WORDS);
    host_dma_buffer[next] = ext_high_ticks;
    host_dma_producer = (uint16_t)((host_dma_producer + 2U) % DMA_WORDS);
    if (host_dma_producer == 0U) host_dma_wrapped = true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 */
void AppPwmInput_TestSignalOverflow(void)
{
    host_timer_overflow = true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 */
void AppPwmInput_TestSignalDmaError(void)
{
    host_dma_error = true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
uint8_t AppPwmInput_TestGetRangeProfile(void)
{
    return (uint8_t)active_profile;
}
#endif

#endif /* engine selection */

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param snap 输出快照指针，成功时写入当前一致性副本。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwmInput_GetSnapshot(AppPwmInputSnapshot *snap)
{
    if (snap == NULL) return false;
    *snap = cached_snap;
    return true;
}
