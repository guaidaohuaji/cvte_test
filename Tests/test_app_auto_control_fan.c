#include "app_auto_control.h"
#include "app_auto_fan_profile.h"
#include "app_damper.h"
#include "app_fan.h"
#include "app_ntc.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t fake_tick;
static AppNtcSnapshot fake_ntc;
static AppFanSnapshot fake_fan;
static DamperSnapshot fake_damper;
static uint16_t last_commanded_duty;
static uint32_t fan_command_count;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

bool AppNtc_GetSnapshot(AppNtcSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = fake_ntc;
    return true;
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
    last_commanded_duty = duty_x100;
    fake_fan.enabled = enabled;
    fake_fan.target_duty_x100 = duty_x100;
    fake_fan.applied_duty_x100 = duty_x100;
    fake_fan.state = enabled ? APP_FAN_STATE_RUNNING : APP_FAN_STATE_OFF;
    return true;
}

bool AppDamper_GetSnapshot(DamperSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = fake_damper;
    return true;
}

uint8_t AppDamper_MoveAbsolute(int32_t target)
{
    fake_damper.target_steps = target;
    fake_damper.current_steps = target;
    return DAMPER_STATUS_OK;
}

uint8_t AppDamper_MoveRelative(int32_t delta)
{
    (void)delta;
    return DAMPER_STATUS_OK;
}

uint8_t AppDamper_SetCurrentPosition(int32_t position)
{
    fake_damper.current_steps = position;
    return DAMPER_STATUS_OK;
}

uint8_t AppDamper_Stop(void)
{
    return DAMPER_STATUS_OK;
}

uint8_t AppDamper_Release(void)
{
    return DAMPER_STATUS_OK;
}

static AppAutoControlSnapshot auto_snapshot(void)
{
    AppAutoControlSnapshot snapshot;
    assert(AppAutoControl_GetSnapshot(&snapshot));
    return snapshot;
}

static void run_at(uint32_t tick)
{
    fake_tick = tick;
    AppAutoControl_Process();
}

int main(void)
{
    AppAutoControlSnapshot snapshot;
    uint16_t initial_duty;
    uint32_t commands_before;

    memset(&fake_ntc, 0, sizeof(fake_ntc));
    memset(&fake_fan, 0, sizeof(fake_fan));
    memset(&fake_damper, 0, sizeof(fake_damper));

    fake_ntc.state = APP_NTC_STATE_OK;
    fake_ntc.range_status = APP_NTC_RANGE_IN_RANGE;
    fake_ntc.sensor_measurement_valid = true;
    fake_ntc.temp_centi_c = 0;
    fake_ntc.control_temp_centi_c = 0;

    fake_damper.status = DAMPER_STATUS_OK;
    fake_damper.damper_state = DAMPER_STATE_IDLE_RELEASED;
    fake_damper.flags = 0x01U;

    fake_tick = 0U;
    AppAutoControl_Init();
    assert(AppAutoControl_SetMode(APP_AUTO_MODE_AUTO) == APP_AUTO_SET_MODE_OK);
    run_at(0U);

    snapshot = auto_snapshot();
    initial_duty = AppAutoFan_EstimateDutyX100(snapshot.target_fan_rpm);
    assert(fan_command_count == 1U);
    assert(last_commanded_duty == initial_duty);
    assert(last_commanded_duty >= APP_AUTO_FAN_NORMAL_MIN_DUTY_X100);
    assert(last_commanded_duty <= APP_AUTO_FAN_NORMAL_MAX_DUTY_X100);

    /* A large error is corrected immediately by 3%, not by the old 1% step. */
    fake_fan.tach_valid = 1U;
    fake_fan.rpm = (uint16_t)(snapshot.target_fan_rpm - 300U);
    run_at(1000U);
    assert(last_commanded_duty == (uint16_t)(initial_duty + 300U));

    /* A medium error needs two same-direction control decisions, then uses 2%. */
    commands_before = fan_command_count;
    fake_fan.rpm = (uint16_t)(snapshot.target_fan_rpm - 200U);
    run_at(2000U);
    assert(fan_command_count == commands_before);
    run_at(3000U);
    assert(fan_command_count == commands_before + 1U);
    assert(last_commanded_duty == (uint16_t)(initial_duty + 500U));

    /* Enter at 50 RPM, remain latched through 80 RPM, then leave above 80. */
    commands_before = fan_command_count;
    fake_fan.rpm = (uint16_t)(snapshot.target_fan_rpm - 40U);
    run_at(4000U);
    snapshot = auto_snapshot();
    assert(snapshot.fan_control_state == APP_AUTO_FAN_CTRL_IN_TOLERANCE);
    assert(fan_command_count == commands_before);

    fake_fan.rpm = (uint16_t)(snapshot.target_fan_rpm - 70U);
    run_at(5000U);
    snapshot = auto_snapshot();
    assert(snapshot.fan_control_state == APP_AUTO_FAN_CTRL_IN_TOLERANCE);
    assert(fan_command_count == commands_before);

    fake_fan.rpm = (uint16_t)(snapshot.target_fan_rpm - 90U);
    run_at(6000U);
    assert(fan_command_count == commands_before);
    run_at(7000U);
    assert(fan_command_count == commands_before + 1U);
    assert(last_commanded_duty == (uint16_t)(initial_duty + 600U));

    /* A large target change re-anchors directly to the interpolation table. */
    fake_ntc.control_temp_centi_c = APP_AUTO_TEMP_MAX_CENTI_C;
    fake_ntc.temp_centi_c = APP_AUTO_TEMP_MAX_CENTI_C;
    commands_before = fan_command_count;
    run_at(8000U);
    snapshot = auto_snapshot();
    assert(snapshot.target_fan_rpm == APP_AUTO_FAN_MAX_RPM);
    assert(fan_command_count == commands_before + 1U);
    assert(last_commanded_duty == APP_AUTO_FAN_NORMAL_MAX_DUTY_X100);

    puts("auto fan closed-loop tests passed");
    return 0;
}
