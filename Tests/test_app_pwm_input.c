#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_pwm_input.h"
#include "app_pwm_input_config.h"
#include "tim.h"

TIM_TypeDef test_tim1;
TIM_HandleTypeDef htim1 = { &test_tim1 };
GPIO_TypeDef test_gpioe;
static uint32_t now_ms;
static uint32_t start_calls;
static bool start_fail;

uint32_t HAL_GetTick(void)
{
    return now_ms;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    return ((port->IDR & pin) != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef *htim, uint32_t channel)
{
    assert(htim == &htim1);
    assert(channel == TIM_CHANNEL_1);
    start_calls++;
    return start_fail ? HAL_ERROR : HAL_OK;
}

static void reset_fixture(bool pin_high)
{
    test_tim1 = (TIM_TypeDef){0};
    test_gpioe.IDR = pin_high ? GPIO_PIN_9 : 0U;
    now_ms = 0U;
    start_calls = 0U;
    start_fail = false;
}

static void capture(uint32_t counter, uint32_t tick_ms)
{
    now_ms = tick_ms;
    test_tim1.SR = 0U;
    test_tim1.CCR1 = counter;
    AppPwmInput_CC_Callback(&htim1);
}

static AppPwmInputSnapshot snapshot(void)
{
    AppPwmInputSnapshot s;
    AppPwmInput_Process();
    assert(AppPwmInput_GetSnapshot(&s));
    return s;
}

static void test_init_and_config_contract(void)
{
    reset_fixture(false);
    assert(APP_PWM_INPUT_TIM_PSC == 7U);
    assert(APP_PWM_INPUT_COUNTER_CLOCK_HZ == 21000000U);
    assert(APP_PWM_INPUT_MIN_FREQ_HZ == 1U);
    assert(APP_PWM_INPUT_LEGACY_MAX_FREQ_HZ == 20000U);
    assert(APP_PWM_INPUT_IC_FILTER == 2U);
    assert(AppPwmInput_Init());
    assert(start_calls == 1U);
    assert((test_tim1.DIER & TIM_IT_UPDATE) != 0U);
}

static void test_20khz_50_percent(void)
{
    AppPwmInputSnapshot s;
    reset_fixture(false);
    assert(AppPwmInput_Init());

    capture(100U, 1U);   /* rising */
    capture(625U, 1U);   /* falling */
    capture(1150U, 1U);  /* rising: period=1050, reported high=525 */

    s = snapshot();
    assert(s.status == APP_PWM_IN_OK);
    assert(s.freq_millihz == 20000000U);
    assert(s.duty_x100 == 5000U);
    assert(s.period_ticks == 1050U);
    assert(s.ext_high_ticks == 525U);
    assert(s.prescaler == 7U);
    assert(s.synced == 1U);
}

static void test_existing_duty_semantics_unchanged(void)
{
    AppPwmInputSnapshot s;
    reset_fixture(false);
    assert(AppPwmInput_Init());

    capture(100U, 1U);   /* rising */
    capture(1675U, 1U);  /* falling */
    capture(2200U, 1U);  /* rising: period=2100, current algorithm high=525 */

    s = snapshot();
    assert(s.status == APP_PWM_IN_OK);
    assert(s.freq_millihz == 10000000U);
    assert(s.duty_x100 == 2500U);
    assert(s.period_ticks == 2100U);
    assert(s.ext_high_ticks == 525U);
}

static void test_above_20khz_rejected(void)
{
    AppPwmInputSnapshot s;
    reset_fixture(false);
    assert(AppPwmInput_Init());

    capture(100U, 1U);
    capture(600U, 1U);
    capture(1100U, 1U); /* period=1000 -> 21 kHz */

    s = snapshot();
    assert(s.status == APP_PWM_IN_SEARCH);
    assert(s.synced == 0U);
}

static void test_overcapture_resynchronizes(void)
{
    AppPwmInputSnapshot s;
    reset_fixture(false);
    assert(AppPwmInput_Init());

    capture(100U, 1U);
    capture(625U, 1U);
    test_tim1.SR = TIM_SR_CC1OF;
    test_tim1.CCR1 = 1150U;
    AppPwmInput_CC_Callback(&htim1);

    s = snapshot();
    assert(s.status == APP_PWM_IN_SEARCH);
    assert(s.synced == 0U);
    assert(s.ovc_count == 1U);
}

static void test_static_level_timeout(void)
{
    AppPwmInputSnapshot s;
    reset_fixture(true);
    assert(AppPwmInput_Init());
    now_ms = APP_PWM_INPUT_TIMEOUT_MS + 1U;
    s = snapshot();
    assert(s.status == APP_PWM_IN_STATIC_HIGH);
    assert(s.raw_pin_level == 1U);
}

static void test_start_failure(void)
{
    reset_fixture(false);
    start_fail = true;
    assert(!AppPwmInput_Init());
    assert((test_tim1.DIER & TIM_IT_UPDATE) == 0U);
}

int main(void)
{
    test_init_and_config_contract();
    test_20khz_50_percent();
    test_existing_duty_semantics_unchanged();
    test_above_20khz_rejected();
    test_overcapture_resynchronizes();
    test_static_level_timeout();
    test_start_failure();
    puts("general PWM input phase-P2 tests passed");
    return 0;
}
