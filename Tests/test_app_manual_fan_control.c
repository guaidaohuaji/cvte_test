#include "app_manual_fan_control.h"

#include "app_auto_control.h"
#include "app_auto_fan_profile.h"
#include "app_fan.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t fake_tick;
static uint8_t fake_auto_mode;
static AppFanSnapshot fake_fan;
static uint32_t fan_command_count;
static bool fan_command_result = true;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

uint8_t AppAutoControl_GetMode(void)
{
    return fake_auto_mode;
}

bool AppFan_GetSnapshot(AppFanSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = fake_fan;
    return true;
}

bool AppFan_SetEnabled(bool enabled, uint16_t duty_x100)
{
    fan_command_count++;
    if (!fan_command_result)
        return false;

    fake_fan.enabled = enabled;
    fake_fan.target_duty_x100 = duty_x100;
    fake_fan.applied_duty_x100 = enabled ? duty_x100 : 0U;
    fake_fan.state = enabled ? APP_FAN_STATE_RUNNING : APP_FAN_STATE_OFF;
    fake_fan.tach_valid = 0U;
    return true;
}

static AppManualFanControlSnapshot snapshot(void)
{
    AppManualFanControlSnapshot s;
    assert(AppManualFanControl_GetSnapshot(&s));
    return s;
}

static void reset_fixture(void)
{
    memset(&fake_fan, 0, sizeof(fake_fan));
    fake_tick = 0U;
    fake_auto_mode = APP_AUTO_MODE_MANUAL;
    fan_command_count = 0U;
    fan_command_result = true;
    AppManualFanControl_Init();
}

static void test_basic_commands_and_lock(void)
{
    AppManualFanControlSnapshot s;

    reset_fixture();
    s = snapshot();
    assert(s.mode == APP_MANUAL_FAN_MODE_OFF);
    assert(s.control_state == APP_MANUAL_FAN_CTRL_INACTIVE);

    assert(AppManualFanControl_SetDuty(3000U) == APP_MANUAL_FAN_RESULT_OK);
    s = snapshot();
    assert(s.mode == APP_MANUAL_FAN_MODE_DUTY);
    assert(fake_fan.target_duty_x100 == 3000U);

    assert(AppManualFanControl_SetOff() == APP_MANUAL_FAN_RESULT_OK);
    s = snapshot();
    assert(s.mode == APP_MANUAL_FAN_MODE_OFF);
    assert(!fake_fan.enabled);

    assert(AppManualFanControl_SetDuty(999U) == APP_MANUAL_FAN_RESULT_INVALID_PARAM);
    assert(AppManualFanControl_SetTargetRpm(999U) == APP_MANUAL_FAN_RESULT_INVALID_PARAM);
    assert(AppManualFanControl_SetTargetRpm(2301U) == APP_MANUAL_FAN_RESULT_INVALID_PARAM);

    fake_auto_mode = APP_AUTO_MODE_AUTO;
    assert(AppManualFanControl_SetDuty(3000U) == APP_MANUAL_FAN_RESULT_MODE_LOCKED);
    assert(AppManualFanControl_SetTargetRpm(1500U) == APP_MANUAL_FAN_RESULT_MODE_LOCKED);
    assert(AppManualFanControl_SetOff() == APP_MANUAL_FAN_RESULT_MODE_LOCKED);
}

static void test_feedforward_and_closed_loop(void)
{
    AppManualFanControlSnapshot s;
    uint16_t ff;
    uint32_t commands_before;

    reset_fixture();
    ff = AppAutoFan_EstimateDutyX100(1500U);
    assert(ff == 2300U);
    assert(AppManualFanControl_SetTargetRpm(1500U) == APP_MANUAL_FAN_RESULT_OK);
    assert(fake_fan.target_duty_x100 == ff);

    s = snapshot();
    assert(s.mode == APP_MANUAL_FAN_MODE_SPEED);
    assert(s.target_rpm == 1500U);
    assert(s.feedforward_duty_x100 == ff);
    assert(s.control_state == APP_MANUAL_FAN_CTRL_STARTING);

    /* AppFan owns the five-second boost; the manual controller must wait. */
    fake_fan.state = APP_FAN_STATE_STARTUP_BOOST;
    fake_fan.enabled = true;
    fake_tick = 4999U;
    commands_before = fan_command_count;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before);
    assert(snapshot().control_state == APP_MANUAL_FAN_CTRL_STARTING);

    /* 300 RPM error is corrected immediately by 3%. */
    fake_fan.state = APP_FAN_STATE_RUNNING;
    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1200U;
    fake_fan.target_duty_x100 = ff;
    fake_fan.applied_duty_x100 = ff;
    fake_tick = 6000U;
    AppManualFanControl_Process();
    assert(fake_fan.target_duty_x100 == (uint16_t)(ff + 300U));
    assert(snapshot().adjust_count == 1U);

    /* A medium error needs two same-direction decisions, then adjusts by 2%. */
    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1300U;
    commands_before = fan_command_count;
    fake_tick = 7000U;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before);
    fake_tick = 8000U;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before + 1U);
    assert(fake_fan.target_duty_x100 == (uint16_t)(ff + 500U));

    /* Enter tolerance at 50 RPM and remain latched until error exceeds 80 RPM. */
    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1460U;
    commands_before = fan_command_count;
    fake_tick = 9000U;
    AppManualFanControl_Process();
    s = snapshot();
    assert(s.control_state == APP_MANUAL_FAN_CTRL_IN_TOLERANCE);
    assert(s.in_tolerance == 1U);
    assert(fan_command_count == commands_before);

    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1430U;
    fake_tick = 10000U;
    AppManualFanControl_Process();
    assert(snapshot().control_state == APP_MANUAL_FAN_CTRL_IN_TOLERANCE);
    assert(fan_command_count == commands_before);

    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1410U;
    fake_tick = 11000U;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before); /* first confirmation */
    fake_tick = 12000U;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before + 1U);
}

static void test_tach_fault_and_recovery(void)
{
    AppManualFanControlSnapshot s;
    uint16_t ff;
    uint32_t commands_before;

    reset_fixture();
    ff = AppAutoFan_EstimateDutyX100(1800U);
    assert(AppManualFanControl_SetTargetRpm(1800U) == APP_MANUAL_FAN_RESULT_OK);

    fake_fan.enabled = true;
    fake_fan.state = APP_FAN_STATE_NO_TACH;
    fake_fan.tach_valid = 0U;
    fake_fan.target_duty_x100 = ff;
    fake_tick = 6000U;
    AppManualFanControl_Process();
    assert(fake_fan.target_duty_x100 == APP_AUTO_FAN_FAILSAFE_DUTY_X100);
    s = snapshot();
    assert(s.control_state == APP_MANUAL_FAN_CTRL_TACH_FAULT);
    assert(s.fault_count == 1U);

    commands_before = fan_command_count;
    AppManualFanControl_Process();
    assert(fan_command_count == commands_before); /* failsafe is not resent */

    fake_fan.state = APP_FAN_STATE_RUNNING;
    fake_fan.tach_valid = 1U;
    fake_fan.rpm = 1800U;
    fake_tick = 7000U;
    AppManualFanControl_Process();
    assert(fake_fan.target_duty_x100 == ff);
    assert(snapshot().control_state == APP_MANUAL_FAN_CTRL_ADJUSTING);
}

static void test_auto_takeover_clears_manual_state(void)
{
    AppManualFanControlSnapshot s;
    uint32_t commands_before;

    reset_fixture();
    assert(AppManualFanControl_SetTargetRpm(1600U) == APP_MANUAL_FAN_RESULT_OK);
    commands_before = fan_command_count;

    fake_auto_mode = APP_AUTO_MODE_AUTO;
    AppManualFanControl_Process();
    s = snapshot();
    assert(s.mode == APP_MANUAL_FAN_MODE_OFF);
    assert(s.control_state == APP_MANUAL_FAN_CTRL_INACTIVE);
    assert(s.target_rpm == 0U);
    assert(fan_command_count == commands_before); /* AUTO ownership is untouched */
}

int main(void)
{
    test_basic_commands_and_lock();
    test_feedforward_and_closed_loop();
    test_tach_fault_and_recovery();
    test_auto_takeover_clears_manual_state();
    puts("manual fan RPM controller phase-1 tests passed");
    return 0;
}
