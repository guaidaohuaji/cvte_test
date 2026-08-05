#include "app_fan_health.h"
#include "app_fan_health_config.h"
#include "app_fan.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t fake_tick;
static AppFanSnapshot fake_fan;
static bool fake_safety_latched;
static bool fake_restart_inhibited;
static uint32_t fake_trip_count;
static uint32_t fake_clear_count;
static uint32_t fake_authorize_count;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

bool AppFan_GetSnapshot(AppFanSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = fake_fan;
    return true;
}

bool AppFan_TripSafetyFault(void)
{
    fake_safety_latched = true;
    fake_restart_inhibited = true;
    fake_trip_count++;
    fake_fan.enabled = false;
    fake_fan.state = APP_FAN_STATE_SAFETY_LOCKED;
    fake_fan.target_duty_x100 = 0U;
    fake_fan.applied_duty_x100 = 0U;
    fake_fan.tach_valid = 0U;
    fake_fan.rpm = 0U;
    return true;
}

bool AppFan_ClearSafetyFault(void)
{
    fake_safety_latched = false;
    fake_restart_inhibited = true;
    fake_clear_count++;
    fake_fan.enabled = false;
    fake_fan.state = APP_FAN_STATE_OFF;
    fake_fan.target_duty_x100 = 0U;
    fake_fan.applied_duty_x100 = 0U;
    return true;
}

bool AppFan_AuthorizeRestart(void)
{
    if (fake_safety_latched)
        return false;
    fake_restart_inhibited = false;
    fake_authorize_count++;
    return true;
}

bool AppFan_IsSafetyFaultLatched(void)
{
    return fake_safety_latched;
}

bool AppFan_IsRestartInhibited(void)
{
    return fake_restart_inhibited;
}

static AppFanHealthSnapshot health(void)
{
    AppFanHealthSnapshot snapshot;
    assert(AppFanHealth_GetSnapshot(&snapshot));
    return snapshot;
}

static void set_fan(bool enabled,
                    AppFanState state,
                    uint16_t applied_duty_x100,
                    bool tach_valid,
                    uint16_t rpm)
{
    memset(&fake_fan, 0, sizeof(fake_fan));
    fake_fan.enabled = enabled;
    fake_fan.state = state;
    fake_fan.applied_duty_x100 = applied_duty_x100;
    fake_fan.target_duty_x100 = applied_duty_x100;
    fake_fan.tach_valid = tach_valid ? 1U : 0U;
    fake_fan.rpm = rpm;
}

static void reset_fixture(void)
{
    memset(&fake_fan, 0, sizeof(fake_fan));
    fake_tick = 0U;
    fake_safety_latched = false;
    fake_restart_inhibited = false;
    fake_trip_count = 0U;
    fake_clear_count = 0U;
    fake_authorize_count = 0U;
    AppFanHealth_Init();
}

static void test_speed_fault_shutdown_and_clear(void)
{
    AppFanHealthSnapshot s;

    reset_fixture();

    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_DISABLED);
    assert(s.fault_latched == 0U);

    /* Startup boost is excluded from diagnosis. */
    set_fan(true, APP_FAN_STATE_STARTUP_BOOST, 10000U, false, 0U);
    fake_tick = 100U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_STARTUP_BOOST);
    assert(s.monitoring_active == 0U);

    /* After boost, the monitor waits five seconds before judging speed. */
    set_fan(true, APP_FAN_STATE_RUNNING, 3500U, true, 1800U);
    fake_tick = 5000U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_SETTLING);
    assert(s.expected_rpm == 1800U);
    assert(s.settling_remaining_ms == 5000U);

    fake_tick = 10000U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_NORMAL);

    /* A 700 RPM low-speed deviation must persist for five seconds. */
    set_fan(true, APP_FAN_STATE_RUNNING, 3500U, true, 1100U);
    fake_tick = 10001U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_SPEED_SUSPECT);
    assert(s.deviation_rpm == -700);
    assert(s.fault_latched == 0U);

    fake_tick = 15000U;
    AppFanHealth_Process();
    assert(health().fault_latched == 0U);

    fake_tick = 15001U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED);
    assert(s.fault_latched == 1U);
    assert(s.fault_type == APP_FAN_HEALTH_FAULT_SPEED_LOW);
    assert(s.fault_applied_duty_x100 == 3500U);
    assert(s.fault_expected_rpm == 1800U);
    assert(s.fault_actual_rpm == 1100U);
    assert(s.fault_deviation_rpm == -700);
    assert(s.shutdown_succeeded == 1U);
    assert(s.restart_inhibited == 1U);
    assert(fake_trip_count == 1U);
    assert(!fake_fan.enabled);
    assert(fake_fan.applied_duty_x100 == 0U);

    /* Fan-off no longer clears a formal fault latch. */
    fake_tick = 16000U;
    AppFanHealth_Process();
    s = health();
    assert(s.fault_latched == 1U);
    assert(s.state == APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED);

    /* Explicit clear keeps the fan off and restart inhibited. */
    assert(AppFanHealth_ClearFault());
    s = health();
    assert(s.fault_latched == 0U);
    assert(s.state == APP_FAN_HEALTH_STATE_DISABLED);
    assert(s.restart_inhibited == 1U);
    assert(fake_clear_count == 1U);
    assert(!fake_fan.enabled);

    assert(AppFanHealth_AuthorizeRestart());
    assert(!AppFanHealth_RestartIsInhibited());
    assert(fake_authorize_count == 1U);
}

static void test_tach_loss_shutdown(void)
{
    AppFanHealthSnapshot s;

    reset_fixture();

    /* First normal command settles for three seconds. */
    set_fan(true, APP_FAN_STATE_RUNNING, 4500U, true, 2000U);
    fake_tick = 20000U;
    AppFanHealth_Process();
    assert(health().state == APP_FAN_HEALTH_STATE_SETTLING);

    fake_tick = 23000U;
    AppFanHealth_Process();
    assert(health().state == APP_FAN_HEALTH_STATE_NORMAL);

    /* Tach loss keeps using the last normal duty even after failsafe 100%. */
    set_fan(true, APP_FAN_STATE_NO_TACH, 4500U, false, 0U);
    fake_tick = 23001U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_TACH_SUSPECT);
    assert(s.reference_duty_x100 == 4500U);

    set_fan(true, APP_FAN_STATE_RUNNING, 10000U, false, 0U);
    fake_tick = 28001U;
    AppFanHealth_Process();
    s = health();
    assert(s.state == APP_FAN_HEALTH_STATE_TACH_FAULT_LATCHED);
    assert(s.fault_latched == 1U);
    assert(s.fault_type == APP_FAN_HEALTH_FAULT_TACH_LOST);
    assert(s.fault_applied_duty_x100 == 10000U);
    assert(s.reference_duty_x100 == 4500U);
    assert(s.fault_expected_rpm == 2000U);
    assert(s.fault_actual_rpm == 0U);
    assert(fake_trip_count == 1U);
    assert(!fake_fan.enabled);
}

int main(void)
{
    assert(APP_FAN_HEALTH_DRIVES_OUTPUT == 1U);
    assert(APP_FAN_HEALTH_DEVIATION_ENTER_RPM == 600U);
    assert(APP_FAN_HEALTH_CONFIRM_MS == 5000U);

    test_speed_fault_shutdown_and_clear();
    test_tach_loss_shutdown();

    puts("fan health formal shutdown phase-2 tests passed");
    return 0;
}
