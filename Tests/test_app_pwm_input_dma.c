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
    (void)htim;
    (void)channel;
    return HAL_ERROR;
}

static void reset_fixture(bool pin_high)
{
    test_tim1 = (TIM_TypeDef){0};
    test_gpioe.IDR = pin_high ? GPIO_PIN_9 : 0U;
    now_ms = 0U;
    assert(AppPwmInput_Init());
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_FAST);
}

static void feed(uint32_t period, uint32_t ext_high, uint32_t tick_ms)
{
    now_ms = tick_ms;
    AppPwmInput_TestFeedDmaPair(period, ext_high, tick_ms);
}

static AppPwmInputSnapshot process_snapshot(void)
{
    AppPwmInputSnapshot snap;
    AppPwmInput_Process();
    assert(AppPwmInput_GetSnapshot(&snap));
    return snap;
}

static void test_200khz_one_percent_fast_profile(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(false);

    feed(105U, 1U, 1U);
    feed(105U, 1U, 1U);
    feed(105U, 1U, 1U);
    feed(105U, 1U, 1U);
    feed(105U, 1U, 1U);
    snap = process_snapshot();

    assert(snap.status == APP_PWM_IN_OK);
    assert(snap.freq_millihz == 200000000U);
    assert(snap.duty_x100 == 95U);
    assert(snap.period_ticks == 105U);
    assert(snap.ext_high_ticks == 1U);
    assert(snap.prescaler == APP_PWM_INPUT_DMA_FAST_PSC);
    assert(snap.range_profile == APP_PWM_INPUT_RANGE_FAST);
    assert(snap.engine_mode == APP_PWM_INPUT_ENGINE_DMA_AUTORANGE);
}

static void test_median_rejects_single_period_spike(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(false);

    feed(2100U, 525U, 1U);
    feed(2100U, 525U, 1U);
    feed(900U, 225U, 1U);
    feed(2100U, 525U, 1U);
    feed(2100U, 525U, 1U);
    snap = process_snapshot();

    assert(snap.status == APP_PWM_IN_OK);
    assert(snap.freq_millihz == 10000000U);
    assert(snap.duty_x100 == 2500U);
}

static void test_downrange_fast_to_medium_to_slow(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(false);

    AppPwmInput_TestSignalOverflow();
    snap = process_snapshot();
    assert(snap.status == APP_PWM_IN_SEARCH);
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_MEDIUM);

    AppPwmInput_TestSignalOverflow();
    snap = process_snapshot();
    assert(snap.status == APP_PWM_IN_SEARCH);
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_SLOW);
}

static void test_one_hz_slow_profile(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(false);

    AppPwmInput_TestSignalOverflow();
    (void)process_snapshot();
    AppPwmInput_TestSignalOverflow();
    (void)process_snapshot();
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_SLOW);

    feed(50000U, 12500U, 1000U);
    feed(50000U, 12500U, 2000U);
    feed(50000U, 12500U, 3000U);
    snap = process_snapshot();

    assert(snap.status == APP_PWM_IN_OK);
    assert(snap.freq_millihz == 1000U);
    assert(snap.duty_x100 == 2500U);
    assert(snap.prescaler == APP_PWM_INPUT_DMA_SLOW_PSC);
}

static void test_uprange_slow_to_medium_to_fast(void)
{
    reset_fixture(false);
    AppPwmInput_TestSignalOverflow();
    (void)process_snapshot();
    AppPwmInput_TestSignalOverflow();
    (void)process_snapshot();
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_SLOW);

    feed(100U, 25U, 10U);
    (void)process_snapshot();
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_MEDIUM);

    feed(100U, 25U, 11U);
    (void)process_snapshot();
    assert(AppPwmInput_TestGetRangeProfile() == APP_PWM_INPUT_RANGE_FAST);
}

static void test_static_level_timeout(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(true);
    now_ms = APP_PWM_INPUT_TIMEOUT_MS + 1U;
    snap = process_snapshot();
    assert(snap.status == APP_PWM_IN_STATIC_HIGH);
    assert(snap.raw_pin_level == 1U);
}

static void test_dma_error_restarts_and_reports(void)
{
    AppPwmInputSnapshot snap;
    reset_fixture(false);
    AppPwmInput_TestSignalDmaError();
    snap = process_snapshot();
    assert(snap.status == APP_PWM_IN_SEARCH || snap.status == APP_PWM_IN_HW_ERROR);
    assert(snap.dma_error_count == 1U || snap.ovc_count == 1U);
}

int main(void)
{
    assert(APP_PWM_INPUT_MAX_FREQ_HZ == 200000U);
    assert(APP_PWM_INPUT_DMA_BUFFER_PAIRS == 128U);
    test_200khz_one_percent_fast_profile();
    test_median_rejects_single_period_spike();
    test_downrange_fast_to_medium_to_slow();
    test_one_hz_slow_profile();
    test_uprange_slow_to_medium_to_fast();
    test_static_level_timeout();
    test_dma_error_restarts_and_reports();
    puts("general PWM input DMA autorange phase-P3 tests passed");
    return 0;
}
