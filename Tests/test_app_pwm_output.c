#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_pwm.h"
#include "tim.h"

static TIM_TypeDef timer4;
TIM_HandleTypeDef htim4 = { &timer4 };
static uint32_t start_calls;
static uint32_t stop_calls;
static bool fail_next_start;

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *handle, uint32_t channel)
{
    assert(handle == &htim4);
    assert(channel == TIM_CHANNEL_1);
    start_calls++;
    if (fail_next_start)
    {
        fail_next_start = false;
        return HAL_ERROR;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef *handle, uint32_t channel)
{
    assert(handle == &htim4);
    assert(channel == TIM_CHANNEL_1);
    stop_calls++;
    return HAL_OK;
}

static uint32_t channel_mode(void)
{
    return timer4.CCMR1 & TIM_CCMR1_OC1M;
}

static void reset_fixture(void)
{
    timer4 = (TIM_TypeDef){0};
    start_calls = 0U;
    stop_calls = 0U;
    fail_next_start = false;
}

static void test_init(void)
{
    reset_fixture();
    assert(AppPwm_Init());
    assert(AppPwm_IsEnabled());
    assert(AppPwm_GetTargetFrequency() == 1000U);
    assert(AppPwm_GetTargetDutyX100() == 5000U);
    assert(AppPwm_GetActualFrequency() == 1000U);
    assert(AppPwm_GetActualDutyX100() == 5000U);
    assert(channel_mode() == TIM_OCMODE_PWM1);
    assert(start_calls == 1U);
    assert(stop_calls == 0U);
}

static void test_100khz_one_percent(void)
{
    uint32_t period;

    assert(AppPwm_Configure(true, 100000U, 100U));
    period = AppPwm_GetAutoReload() + 1U;
    assert(AppPwm_GetPrescaler() == 0U);
    assert(period == 840U);
    assert(AppPwm_GetCompare() == 8U);
    assert(AppPwm_GetTargetDutyX100() == 100U);
    assert(AppPwm_GetActualDutyX100() == 95U);
    assert(channel_mode() == TIM_OCMODE_PWM1);
}

static void test_duty_quantization(void)
{
    assert(AppPwm_Configure(true, 10000U, 149U));
    assert(AppPwm_GetTargetDutyX100() == 100U);
    assert(AppPwm_Configure(true, 10000U, 150U));
    assert(AppPwm_GetTargetDutyX100() == 200U);
    assert(AppPwm_Configure(true, 10000U, 9950U));
    assert(AppPwm_GetTargetDutyX100() == 10000U);
}

static void test_forced_levels(void)
{
    assert(AppPwm_Configure(true, 840000U, 0U));
    assert(AppPwm_GetAutoReload() + 1U == 100U);
    assert(AppPwm_GetActualDutyX100() == 0U);
    assert(channel_mode() == TIM_OCMODE_FORCED_INACTIVE);

    assert(AppPwm_Configure(true, 840000U, 10000U));
    assert(AppPwm_GetActualDutyX100() == 10000U);
    assert(channel_mode() == TIM_OCMODE_FORCED_ACTIVE);
}

static void test_frequency_boundaries(void)
{
    assert(AppPwm_Configure(true, 1U, 5000U));
    assert(AppPwm_GetAutoReload() <= 65535U);
    assert(AppPwm_GetPrescaler() <= 65535U);
    assert(AppPwm_Configure(true, 840000U, 5000U));
    assert(AppPwm_GetAutoReload() + 1U == 100U);
    assert(!AppPwm_Configure(true, 0U, 5000U));
    assert(!AppPwm_Configure(true, 840001U, 5000U));
    assert(!AppPwm_Configure(true, 1000U, 10001U));
}

static void test_disable_preserves_target(void)
{
    uint32_t target_freq;
    uint16_t target_duty;

    assert(AppPwm_Configure(true, 12345U, 3300U));
    target_freq = AppPwm_GetTargetFrequency();
    target_duty = AppPwm_GetTargetDutyX100();
    assert(AppPwm_Configure(false, 0U, 0U));
    assert(!AppPwm_IsEnabled());
    assert(channel_mode() == TIM_OCMODE_FORCED_INACTIVE);
    assert(AppPwm_GetActualDutyX100() == 0U);
    assert(AppPwm_GetTargetFrequency() == target_freq);
    assert(AppPwm_GetTargetDutyX100() == target_duty);
    assert(!AppPwm_Configure(false, 1U, 0U));
}

static void test_atomic_single_restart(void)
{
    uint32_t starts_before = start_calls;
    uint32_t stops_before = stop_calls;

    assert(AppPwm_Configure(true, 200000U, 2500U));
    assert(start_calls == starts_before + 1U);
    assert(stop_calls == stops_before + 1U);
}

static void test_failed_start_rolls_back(void)
{
    uint32_t old_psc = AppPwm_GetPrescaler();
    uint32_t old_arr = AppPwm_GetAutoReload();
    uint32_t old_ccr = AppPwm_GetCompare();
    uint32_t old_mode = channel_mode();
    uint32_t old_target_freq = AppPwm_GetTargetFrequency();
    uint16_t old_target_duty = AppPwm_GetTargetDutyX100();

    fail_next_start = true;
    assert(!AppPwm_Configure(true, 300000U, 6000U));
    assert(timer4.PSC == old_psc);
    assert(timer4.ARR == old_arr);
    assert(timer4.CCR1 == old_ccr);
    assert(channel_mode() == old_mode);
    assert(AppPwm_GetTargetFrequency() == old_target_freq);
    assert(AppPwm_GetTargetDutyX100() == old_target_duty);
}

int main(void)
{
    test_init();
    test_100khz_one_percent();
    test_duty_quantization();
    test_forced_levels();
    test_frequency_boundaries();
    test_disable_preserves_target();
    test_atomic_single_restart();
    test_failed_start_rolls_back();
    puts("general PWM output phase-P1 tests passed");
    return 0;
}
