#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_damper.h"
#include "main.h"
#include "tim.h"

GPIO_TypeDef test_gpio_b;
GPIO_TypeDef test_gpio_c;
TIM_TypeDef test_tim6_instance;
TIM_HandleTypeDef htim6 = { &test_tim6_instance };

static uint32_t fake_tick;
static uint32_t primask_state;
static uint32_t gpio_init_calls;
static uint32_t nvic_clear_calls;

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint32_t pins, GPIO_PinState state)
{
    (void)port;
    (void)pins;
    (void)state;
}

void HAL_GPIO_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init_cfg)
{
    (void)port;
    (void)init_cfg;
    gpio_init_calls++;
}

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

uint32_t __get_PRIMASK(void)
{
    return primask_state;
}

void __disable_irq(void)
{
    primask_state = 1U;
}

void __enable_irq(void)
{
    primask_state = 0U;
}

void HAL_NVIC_ClearPendingIRQ(int irqn)
{
    assert(irqn == TIM6_DAC_IRQn);
    nvic_clear_calls++;
}

static DamperSnapshot snapshot(void)
{
    DamperSnapshot snap;
    assert(AppDamper_GetSnapshot(&snap));
    return snap;
}

static void reset_fixture(void)
{
    test_gpio_b = (GPIO_TypeDef){0};
    test_gpio_c = (GPIO_TypeDef){0};
    test_tim6_instance = (TIM_TypeDef){0};
    htim6.Instance = TIM6;
    fake_tick = 0U;
    primask_state = 0U;
    gpio_init_calls = 0U;
    nvic_clear_calls = 0U;
}

static void start_fresh_homing(void)
{
    reset_fixture();
    AppDamper_Init();
}

static void finish_homing_motion(void)
{
    uint32_t i;
    for (i = 0U; i < APP_DAMPER_BOOT_HOMING_STEPS; ++i)
    {
        AppDamper_TimerCallback();
    }
}

static void finish_homing_and_release(void)
{
    finish_homing_motion();
    fake_tick += APP_DAMPER_POST_MOVE_HOLD_MS;
    AppDamper_Process();
}

static void test_init_starts_open_direction_homing(void)
{
    DamperSnapshot snap;

    start_fresh_homing();
    snap = snapshot();

    assert(gpio_init_calls == 2U);
    assert(nvic_clear_calls >= 1U);
    assert(snap.damper_state == DAMPER_STATE_BOOT_HOMING);
    assert((snap.flags & 0x01U) == 0U); /* position invalid */
    assert((snap.flags & 0x02U) != 0U); /* motion active */
    assert((snap.flags & 0x04U) == 0U); /* reverse/open direction */
    assert((snap.flags & 0x08U) == 0U); /* energized */
    assert((snap.flags & 0x20U) != 0U); /* boot homing */
    assert(snap.current_steps == 0);
    assert(snap.target_steps == 0);
    assert(snap.remaining_steps == APP_DAMPER_BOOT_HOMING_STEPS);
    assert(snap.last_command == DAMPER_CMD_BOOT_HOMING);
    assert(snap.last_result == DAMPER_RESULT_IN_PROGRESS);
    assert((test_tim6_instance.CR1 & 1U) != 0U);
    assert((test_tim6_instance.DIER & TIM_IT_UPDATE) != 0U);
}

static void test_commands_are_busy_during_homing(void)
{
    start_fresh_homing();
    assert(AppDamper_MoveAbsolute(100) == DAMPER_STATUS_BUSY);
    assert(AppDamper_MoveRelative(10) == DAMPER_STATUS_BUSY);
    assert(AppDamper_SetCurrentPosition(0) == DAMPER_STATUS_BUSY);
}

static void test_homing_establishes_open_reference(void)
{
    DamperSnapshot snap;
    uint32_t i;

    start_fresh_homing();
    for (i = 0U; i + 1U < APP_DAMPER_BOOT_HOMING_STEPS; ++i)
    {
        AppDamper_TimerCallback();
    }

    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_BOOT_HOMING);
    assert(snap.remaining_steps == 1U);
    assert((snap.flags & 0x01U) == 0U);

    AppDamper_TimerCallback();
    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_POST_MOVE_HOLD);
    assert(snap.remaining_steps == 0U);
    assert(snap.current_steps == 0);
    assert(snap.target_steps == 0);
    assert((snap.flags & 0x01U) != 0U);
    assert((snap.flags & 0x10U) != 0U);
    assert((snap.flags & 0x20U) != 0U);
    assert(snap.last_result == DAMPER_RESULT_SUCCESS);

    fake_tick += APP_DAMPER_POST_MOVE_HOLD_MS - 1U;
    AppDamper_Process();
    assert(snapshot().damper_state == DAMPER_STATE_POST_MOVE_HOLD);

    fake_tick += 1U;
    AppDamper_Process();
    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_IDLE_RELEASED);
    assert((snap.flags & 0x01U) != 0U);
    assert((snap.flags & 0x08U) != 0U);
    assert((snap.flags & 0x20U) == 0U);
}

static void test_absolute_control_after_homing(void)
{
    DamperSnapshot snap;

    start_fresh_homing();
    finish_homing_and_release();
    assert(AppDamper_MoveAbsolute((int32_t)APP_DAMPER_FULL_TRAVEL_STEPS) ==
           DAMPER_STATUS_OK);

    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_MOVING_FORWARD);
    assert((snap.flags & 0x04U) != 0U);
    assert(snap.target_steps == (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS);
    assert(snap.remaining_steps == APP_DAMPER_FULL_TRAVEL_STEPS);
}

static void test_stop_aborts_homing_without_reference(void)
{
    DamperSnapshot snap;

    start_fresh_homing();
    AppDamper_TimerCallback();
    assert(AppDamper_Stop() == DAMPER_STATUS_OK);

    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_STOPPED);
    assert((snap.flags & 0x01U) == 0U);
    assert((snap.flags & 0x20U) == 0U);
    assert(snap.remaining_steps == 0U);
    assert(snap.last_result == DAMPER_RESULT_ABORTED_BY_STOP);
    assert(AppDamper_MoveAbsolute(100) == DAMPER_STATUS_NO_VALID_DATA);
}

static void test_release_aborts_homing_without_reference(void)
{
    DamperSnapshot snap;

    start_fresh_homing();
    assert(AppDamper_Release() == DAMPER_STATUS_OK);
    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_IDLE_RELEASED);
    assert((snap.flags & 0x01U) == 0U);
    assert((snap.flags & 0x20U) == 0U);
    assert(snap.last_result == DAMPER_RESULT_ABORTED_BY_RELEASE);
}

static void test_emergency_aborts_homing(void)
{
    DamperSnapshot snap;

    start_fresh_homing();
    AppDamper_EmergencyShutdown();
    snap = snapshot();
    assert(snap.damper_state == DAMPER_STATE_FAULT);
    assert((snap.flags & 0x01U) == 0U);
    assert((snap.flags & 0x20U) == 0U);
    assert((snap.fault_flags & 0x01U) != 0U);
}

int main(void)
{
    test_init_starts_open_direction_homing();
    test_commands_are_busy_during_homing();
    test_homing_establishes_open_reference();
    test_absolute_control_after_homing();
    test_stop_aborts_homing_without_reference();
    test_release_aborts_homing_without_reference();
    test_emergency_aborts_homing();
    puts("damper boot-open homing step-24 tests passed");
    return 0;
}
