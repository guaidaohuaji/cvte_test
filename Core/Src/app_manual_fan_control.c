#include "app_manual_fan_control.h"

#include "app_auto_control.h"
#include "app_auto_fan_profile.h"
#include "app_fan.h"
#include "app_fan_config.h"
#include "stm32f4xx_hal.h"

#include <limits.h>
#include <stddef.h>

static AppManualFanControlSnapshot snap;
static int8_t error_confirm_direction;
static uint8_t error_confirm_count;
static uint32_t last_adjust_tick;

static bool manual_mode_is_allowed(void)
{
    return AppAutoControl_GetMode() == APP_AUTO_MODE_MANUAL;
}

static uint16_t abs_error_rpm(int32_t error)
{
    uint32_t magnitude = (error < 0) ? (uint32_t)(-error) : (uint32_t)error;
    return (magnitude > UINT16_MAX) ? UINT16_MAX : (uint16_t)magnitude;
}

static void reset_error_confirmation(void)
{
    error_confirm_direction = 0;
    error_confirm_count = 0U;
}

static void clear_speed_state(void)
{
    snap.control_state = APP_MANUAL_FAN_CTRL_INACTIVE;
    snap.target_rpm = 0U;
    snap.rpm_error = 0;
    snap.tach_valid = 0U;
    snap.in_tolerance = 0U;
    snap.feedforward_duty_x100 = 0U;
    reset_error_confirmation();
    last_adjust_tick = 0U;
}

static bool error_direction_is_confirmed(int8_t direction,
                                         uint16_t absolute_error_rpm)
{
    if (absolute_error_rpm > APP_AUTO_FAN_MEDIUM_ERROR_MAX_RPM)
    {
        reset_error_confirmation();
        return true;
    }

    if (error_confirm_direction != direction)
    {
        error_confirm_direction = direction;
        error_confirm_count = 1U;
        return APP_AUTO_FAN_ERROR_CONFIRM_CYCLES <= 1U;
    }

    if (error_confirm_count < UINT8_MAX)
        error_confirm_count++;

    if (error_confirm_count >= APP_AUTO_FAN_ERROR_CONFIRM_CYCLES)
    {
        reset_error_confirmation();
        return true;
    }

    return false;
}

static AppManualFanResult apply_normal_duty(uint16_t duty_x100)
{
    if (duty_x100 < APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        duty_x100 = APP_AUTO_FAN_NORMAL_MIN_DUTY_X100;
    if (duty_x100 > APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        duty_x100 = APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;

    if (!AppFan_SetEnabled(true, duty_x100))
    {
        snap.control_state = APP_MANUAL_FAN_CTRL_HW_ERROR;
        return APP_MANUAL_FAN_RESULT_HW_ERROR;
    }

    return APP_MANUAL_FAN_RESULT_OK;
}

void AppManualFanControl_Init(void)
{
    snap.mode = APP_MANUAL_FAN_MODE_OFF;
    snap.adjust_count = 0U;
    snap.fault_count = 0U;
    clear_speed_state();
}

AppManualFanResult AppManualFanControl_SetOff(void)
{
    if (!manual_mode_is_allowed())
        return APP_MANUAL_FAN_RESULT_MODE_LOCKED;

    if (!AppFan_SetEnabled(false, 0U))
        return APP_MANUAL_FAN_RESULT_HW_ERROR;

    snap.mode = APP_MANUAL_FAN_MODE_OFF;
    clear_speed_state();
    return APP_MANUAL_FAN_RESULT_OK;
}

AppManualFanResult AppManualFanControl_SetDuty(uint16_t duty_x100)
{
    if (!manual_mode_is_allowed())
        return APP_MANUAL_FAN_RESULT_MODE_LOCKED;

    if ((duty_x100 != 0U) &&
        ((duty_x100 < APP_FAN_MIN_DUTY_X100) ||
         (duty_x100 > APP_FAN_MAX_DUTY_X100)))
    {
        return APP_MANUAL_FAN_RESULT_INVALID_PARAM;
    }

    if (!AppFan_SetEnabled(true, duty_x100))
        return APP_MANUAL_FAN_RESULT_HW_ERROR;

    snap.mode = APP_MANUAL_FAN_MODE_DUTY;
    clear_speed_state();
    return APP_MANUAL_FAN_RESULT_OK;
}

AppManualFanResult AppManualFanControl_SetTargetRpm(uint16_t target_rpm)
{
    uint16_t feedforward_duty;
    AppManualFanResult result;

    if (!manual_mode_is_allowed())
        return APP_MANUAL_FAN_RESULT_MODE_LOCKED;

    if ((target_rpm < APP_AUTO_FAN_MIN_RPM) ||
        (target_rpm > APP_AUTO_FAN_MAX_RPM))
    {
        return APP_MANUAL_FAN_RESULT_INVALID_PARAM;
    }

    feedforward_duty = AppAutoFan_EstimateDutyX100(target_rpm);
    result = apply_normal_duty(feedforward_duty);
    if (result != APP_MANUAL_FAN_RESULT_OK)
        return result;

    snap.mode = APP_MANUAL_FAN_MODE_SPEED;
    snap.target_rpm = target_rpm;
    snap.rpm_error = 0;
    snap.tach_valid = 0U;
    snap.in_tolerance = 0U;
    snap.feedforward_duty_x100 = feedforward_duty;
    snap.control_state = APP_MANUAL_FAN_CTRL_STARTING;
    reset_error_confirmation();
    last_adjust_tick = HAL_GetTick();

    return APP_MANUAL_FAN_RESULT_OK;
}

void AppManualFanControl_Process(void)
{
    AppFanSnapshot fan;
    uint32_t now;
    int32_t error;
    uint16_t absolute_error;
    uint16_t trim_step;
    uint16_t commanded_duty;
    uint16_t new_duty;
    int8_t direction;
    bool was_in_tolerance;

    if (!manual_mode_is_allowed())
    {
        /* AUTO owns the fan. Do not restore an old manual command on return. */
        snap.mode = APP_MANUAL_FAN_MODE_OFF;
        clear_speed_state();
        return;
    }

    if (snap.mode != APP_MANUAL_FAN_MODE_SPEED)
        return;

    if (!AppFan_GetSnapshot(&fan))
    {
        snap.control_state = APP_MANUAL_FAN_CTRL_HW_ERROR;
        return;
    }

    now = HAL_GetTick();
    snap.tach_valid = fan.tach_valid ? 1U : 0U;

    if (!fan.enabled)
    {
        if (apply_normal_duty(snap.feedforward_duty_x100) == APP_MANUAL_FAN_RESULT_OK)
        {
            snap.control_state = APP_MANUAL_FAN_CTRL_STARTING;
            last_adjust_tick = now;
        }
        return;
    }

    if (fan.state == APP_FAN_STATE_STARTUP_BOOST)
    {
        snap.control_state = APP_MANUAL_FAN_CTRL_STARTING;
        snap.in_tolerance = 0U;
        last_adjust_tick = now;
        return;
    }

    if (fan.state == APP_FAN_STATE_NO_TACH)
    {
        if (fan.target_duty_x100 != APP_AUTO_FAN_FAILSAFE_DUTY_X100)
        {
            if (AppFan_SetEnabled(true, APP_AUTO_FAN_FAILSAFE_DUTY_X100) &&
                snap.fault_count != UINT32_MAX)
            {
                snap.fault_count++;
            }
        }
        snap.control_state = APP_MANUAL_FAN_CTRL_TACH_FAULT;
        snap.in_tolerance = 0U;
        reset_error_confirmation();
        last_adjust_tick = now;
        return;
    }

    if (snap.control_state == APP_MANUAL_FAN_CTRL_TACH_FAULT)
    {
        if (!fan.tach_valid)
            return;

        if (apply_normal_duty(snap.feedforward_duty_x100) != APP_MANUAL_FAN_RESULT_OK)
            return;

        snap.control_state = APP_MANUAL_FAN_CTRL_ADJUSTING;
        reset_error_confirmation();
        last_adjust_tick = now;
        return;
    }

    if (!fan.tach_valid)
    {
        snap.control_state = APP_MANUAL_FAN_CTRL_WAIT_TACH;
        snap.in_tolerance = 0U;
        reset_error_confirmation();
        return;
    }

    error = (int32_t)snap.target_rpm - (int32_t)fan.rpm;
    if (error > INT16_MAX)
        snap.rpm_error = INT16_MAX;
    else if (error < INT16_MIN)
        snap.rpm_error = INT16_MIN;
    else
        snap.rpm_error = (int16_t)error;

    if ((uint32_t)(now - last_adjust_tick) < APP_AUTO_FAN_CONTROL_PERIOD_MS)
        return;

    last_adjust_tick = now;
    absolute_error = abs_error_rpm(error);
    was_in_tolerance = (snap.control_state == APP_MANUAL_FAN_CTRL_IN_TOLERANCE);

    if (AppAutoFan_ErrorIsInTolerance(absolute_error, was_in_tolerance))
    {
        snap.control_state = APP_MANUAL_FAN_CTRL_IN_TOLERANCE;
        snap.in_tolerance = 1U;
        reset_error_confirmation();
        return;
    }

    snap.in_tolerance = 0U;
    snap.control_state = APP_MANUAL_FAN_CTRL_ADJUSTING;
    direction = (error > 0) ? 1 : -1;

    if (!error_direction_is_confirmed(direction, absolute_error))
        return;

    trim_step = AppAutoFan_SelectTrimStepX100(absolute_error);
    commanded_duty = fan.target_duty_x100;

    if (direction > 0)
    {
        if (commanded_duty >= APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        {
            snap.control_state = APP_MANUAL_FAN_CTRL_SATURATED_HIGH;
            return;
        }
        new_duty = (uint16_t)(commanded_duty + trim_step);
        if ((new_duty < commanded_duty) ||
            (new_duty > APP_AUTO_FAN_NORMAL_MAX_DUTY_X100))
        {
            new_duty = APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;
        }
    }
    else
    {
        if (commanded_duty <= APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        {
            snap.control_state = APP_MANUAL_FAN_CTRL_SATURATED_LOW;
            return;
        }
        if (commanded_duty <= APP_AUTO_FAN_NORMAL_MIN_DUTY_X100 + trim_step)
            new_duty = APP_AUTO_FAN_NORMAL_MIN_DUTY_X100;
        else
            new_duty = (uint16_t)(commanded_duty - trim_step);
    }

    if ((new_duty != commanded_duty) &&
        (apply_normal_duty(new_duty) == APP_MANUAL_FAN_RESULT_OK) &&
        (snap.adjust_count != UINT32_MAX))
    {
        snap.adjust_count++;
    }
}

bool AppManualFanControl_GetSnapshot(AppManualFanControlSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = snap;
    return true;
}
