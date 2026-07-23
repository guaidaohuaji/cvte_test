#include "app_pwm.h"
#include "tim.h"

#define TIM4_CLOCK_HZ 84000000ULL

static uint32_t target_freq    = 1000U;
static uint32_t actual_freq    = 1000U;
static int32_t  freq_ppm       = 0;
static uint16_t target_duty_x100 = 5000U;
static uint16_t actual_duty_x100 = 5000U;
static uint32_t current_psc    = 83U;
static uint32_t current_arr    = 999U;
static uint32_t current_ccr    = 500U;
static bool     pwm_enabled    = true;
static bool     pwm_running    = true;

static bool compute_frequency_params(uint32_t hz,
                                     uint32_t *psc_out,
                                     uint32_t *arr_out)
{
    if ((hz < 1U) || (hz > 100000U)) { return false; }

    uint64_t max_period_den = (uint64_t)hz * 65535ULL;
    uint64_t psc_div = (TIM4_CLOCK_HZ + max_period_den - 1ULL) / max_period_den;

    if (psc_div < 1ULL) { psc_div = 1ULL; }
    if (psc_div > 65536ULL) { return false; }

    uint64_t denom = (uint64_t)hz * psc_div;
    uint64_t arr_div = (TIM4_CLOCK_HZ + denom / 2ULL) / denom;

    if (arr_div < 2ULL)
    {
        if (psc_div >= 65536ULL) { return false; }
        psc_div++;
        denom = (uint64_t)hz * psc_div;
        arr_div = (TIM4_CLOCK_HZ + denom / 2ULL) / denom;
    }

    if ((arr_div < 2ULL) || (arr_div > 65535ULL)) { return false; }
    if ((psc_div < 1ULL) || (psc_div > 65536ULL)) { return false; }

    *psc_out = (uint32_t)(psc_div - 1ULL);
    *arr_out = (uint32_t)(arr_div - 1ULL);
    return true;
}

static uint32_t compute_ccr_x100(uint32_t arr, uint16_t duty_x100)
{
    uint64_t period = (uint64_t)arr + 1ULL;
    uint64_t ccr = (period * (uint64_t)duty_x100 + 5000ULL) / 10000ULL;
    if (ccr > 65535ULL) { ccr = 65535ULL; }
    return (uint32_t)ccr;
}

static bool AppPwm_Apply(uint32_t freq_hz, uint16_t duty_x100)
{
    if (duty_x100 > 10000U) { return false; }

    uint32_t new_psc, new_arr;
    if (!compute_frequency_params(freq_hz, &new_psc, &new_arr)) { return false; }

    uint32_t new_ccr = compute_ccr_x100(new_arr, duty_x100);

    uint64_t actual_div = (uint64_t)(new_psc + 1U) * (uint64_t)(new_arr + 1U);
    uint32_t new_actual_freq =
        (uint32_t)((TIM4_CLOCK_HZ + actual_div / 2ULL) / actual_div);

    uint64_t period_counts = (uint64_t)new_arr + 1ULL;
    uint16_t new_duty_x100 =
        (uint16_t)(((uint64_t)new_ccr * 10000ULL + period_counts / 2ULL) /
                    period_counts);

    int64_t e_ppm = (int64_t)new_actual_freq - (int64_t)freq_hz;
    e_ppm = (e_ppm * 1000000LL) / (int64_t)freq_hz;
    int32_t new_ppm = (int32_t)e_ppm;

    uint32_t old_psc = current_psc;
    uint32_t old_arr = current_arr;
    uint32_t old_ccr = current_ccr;

    HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1);

    __HAL_TIM_SET_PRESCALER(&htim4, new_psc);
    __HAL_TIM_SET_AUTORELOAD(&htim4, new_arr);
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, new_ccr);
    __HAL_TIM_SET_COUNTER(&htim4, 0U);

    htim4.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);

    if (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_SET_PRESCALER(&htim4, old_psc);
        __HAL_TIM_SET_AUTORELOAD(&htim4, old_arr);
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, old_ccr);
        __HAL_TIM_SET_COUNTER(&htim4, 0U);
        htim4.Instance->EGR = TIM_EGR_UG;
        __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);
        HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);
        return false;
    }

    target_freq      = freq_hz;
    actual_freq      = new_actual_freq;
    freq_ppm         = new_ppm;
    target_duty_x100 = duty_x100;
    actual_duty_x100 = new_duty_x100;
    current_psc      = new_psc;
    current_arr      = new_arr;
    current_ccr      = new_ccr;
    pwm_running      = true;

    return true;
}

bool AppPwm_Init(void)
{
    return AppPwm_Apply(1000U, 5000U);
}

bool AppPwm_SetFrequency(uint32_t frequency_hz)
{
    return AppPwm_Apply(frequency_hz, target_duty_x100);
}

bool AppPwm_SetDutyX100(uint16_t duty_x100)
{
    return AppPwm_Apply(target_freq, duty_x100);
}

bool AppPwm_Enable(bool enabled)
{
    pwm_enabled = enabled;

    if (enabled)
    {
        return AppPwm_Apply(target_freq, target_duty_x100);
    }
    else
    {
        if (!pwm_running) return true;
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0U);
        current_ccr  = 0U;
        actual_duty_x100 = 0U;
        pwm_running  = true;
        return true;
    }
}

bool AppPwm_IsEnabled(void) { return pwm_enabled; }

uint32_t AppPwm_GetTargetFrequency(void)   { return target_freq; }
uint32_t AppPwm_GetActualFrequency(void)   { return actual_freq; }
int32_t  AppPwm_GetFrequencyErrorPpm(void) { return freq_ppm; }

uint16_t AppPwm_GetTargetDutyX100(void) { return target_duty_x100; }
uint16_t AppPwm_GetActualDutyX100(void) { return actual_duty_x100; }

uint32_t AppPwm_GetPrescaler(void)  { return current_psc; }
uint32_t AppPwm_GetAutoReload(void) { return current_arr; }
uint32_t AppPwm_GetCompare(void)    { return current_ccr; }
