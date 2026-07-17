#include "app_pwm_input.h"
#include "tim.h"

#define INPUT_INVERTED      0
#define TIMEOUT_MS          2500U
#define MAX_FREQ_HZ         5000U
#define MIN_FREQ_HZ         1U
#define COUNTER_CLOCK_HZ    1000000U

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
static uint32_t cached_age_ms;

static bool check_freq_range(uint32_t period_ticks)
{
    if (period_ticks == 0U) return false;
    uint64_t hz1000 = (uint64_t)COUNTER_CLOCK_HZ * 1000ULL / period_ticks;
    return (hz1000 >= ((uint64_t)MIN_FREQ_HZ * 1000ULL)) &&
           (hz1000 <= ((uint64_t)MAX_FREQ_HZ * 1000ULL));
}

bool AppPwmInput_Init(void)
{
    __HAL_TIM_DISABLE(&htim1);
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);

    __HAL_TIM_SET_COUNTER(&htim1, 0U);
    htim1.Instance->EGR = TIM_EGR_UG;

    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_UPDATE);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_CC1);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_CC1OF);

    overflow_count = 0U;

    synchronized  = false;
    rise_valid    = false;
    fall_valid    = false;
    ovc_count     = 0U;
    raw_valid     = false;
    raw_synced    = false;
    capture_seq   = 0U;

    expected_edge = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_SET)
                    ? EXPECT_FALLING : EXPECT_RISING;

    cached_snap.status = APP_PWM_IN_SEARCH;
    cached_snap.synced = 0;

    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);

    if (HAL_TIM_IC_Start_IT(&htim1, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE);
        return false;
    }

    return true;
}

void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM1) return;

    uint32_t sr = htim->Instance->SR;

    if ((sr & TIM_SR_CC1OF) != 0U)
    {
        htim->Instance->SR = ~TIM_SR_CC1OF;
        ovc_count++;
        raw_ovc = ovc_count;
        synchronized = false;
        rise_valid   = false;
        fall_valid   = false;
        raw_synced   = false;
        expected_edge = (HAL_GPIO_ReadPin(GPIOE, GPIO_PIN_9) == GPIO_PIN_SET)
                        ? EXPECT_FALLING : EXPECT_RISING;
        return;
    }

    uint32_t ccr = htim->Instance->CCR1;
    uint32_t ovf = overflow_count;

    if (((sr & TIM_SR_UIF) != 0U) && (ccr < 0x8000U))
    {
        ovf = ovf + 1U;
    }

    uint64_t ts = ((uint64_t)ovf << 16) | (uint64_t)ccr;
    last_cap_ms_hw = HAL_GetTick();

    if (expected_edge == EXPECT_RISING)
    {
        if (rise_valid && fall_valid)
        {
            uint64_t period = ts - last_rise_ts;
            uint64_t high   = ts - last_fall_ts;

            if ((period > 0ULL) && (period < 0x100000000ULL) &&
                (high <= period) &&
                check_freq_range((uint32_t)period))
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

void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM1) return;
    overflow_count++;
}

void AppPwmInput_Process(void)
{
    uint32_t seq = capture_seq;
    bool valid   = raw_valid;
    uint32_t p   = raw_period_ticks;
    uint32_t h   = raw_high_ticks;
    uint32_t ov  = raw_ovc;
    uint32_t cap_ms = raw_last_cap_ms;
    bool syn     = raw_synced;
    if (seq != capture_seq) return;

    uint32_t now = HAL_GetTick();
    uint32_t age = (now >= cap_ms) ? (now - cap_ms) : 0U;
    uint8_t pin_lvl = (GPIOE->IDR & GPIO_PIN_9) ? 1U : 0U;

    if (!valid || !syn)
    {
        if (age > TIMEOUT_MS)
        {
            if (pin_lvl) {
                cached_snap.status = INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_LOW : APP_PWM_IN_STATIC_HIGH;
            } else {
                cached_snap.status = INPUT_INVERTED ?
                    APP_PWM_IN_STATIC_HIGH : APP_PWM_IN_STATIC_LOW;
            }
            cached_snap.raw_pin_level = pin_lvl;
            cached_snap.synced = 0;
            cached_snap.age_ms = age;
            cached_snap.ovc_count = ovc_count;
            cached_age_ms = now;
        }
        else
        {
            cached_snap.status = APP_PWM_IN_SEARCH;
            cached_snap.raw_pin_level = pin_lvl;
            cached_snap.synced = 0;
            cached_snap.age_ms = age;
            cached_snap.ovc_count = ovc_count;
            cached_age_ms = now;
        }
        return;
    }

    if (age > TIMEOUT_MS)
    {
        if (pin_lvl) {
            cached_snap.status = INPUT_INVERTED ?
                APP_PWM_IN_STATIC_LOW : APP_PWM_IN_STATIC_HIGH;
        } else {
            cached_snap.status = INPUT_INVERTED ?
                APP_PWM_IN_STATIC_HIGH : APP_PWM_IN_STATIC_LOW;
        }
        cached_snap.raw_pin_level = pin_lvl;
        cached_snap.synced = 0;
        cached_snap.age_ms = age;
        cached_snap.ovc_count = ov;
        cached_age_ms = now;
        return;
    }

    uint64_t fm1000 = (uint64_t)COUNTER_CLOCK_HZ * 1000ULL / (uint64_t)p;

#if INPUT_INVERTED
    uint64_t ext_high = (uint64_t)p - h;
#else
    uint64_t ext_high = h;
#endif

    uint64_t dx100 = (ext_high * 10000ULL + (uint64_t)p / 2ULL) / (uint64_t)p;
    if (dx100 > 10000ULL) dx100 = 10000ULL;

    cached_snap.status        = APP_PWM_IN_OK;
    cached_snap.freq_millihz  = (uint32_t)fm1000;
    cached_snap.duty_x100     = (uint32_t)dx100;
    cached_snap.period_ticks  = p;
    cached_snap.ext_high_ticks = (uint32_t)ext_high;
    cached_snap.age_ms        = age;
    cached_snap.ovc_count     = ov;
    cached_snap.prescaler     = 167;
    cached_snap.synced        = 1;
    cached_snap.raw_pin_level = pin_lvl;
    cached_age_ms = now;
}

bool AppPwmInput_GetSnapshot(AppPwmInputSnapshot *snap)
{
    if (snap == NULL) return false;
    *snap = cached_snap;
    return true;
}
