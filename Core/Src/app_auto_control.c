#include "app_auto_control.h"
#include "app_ntc.h"
#include "app_fan.h"
#include "app_damper.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

#define TEMP_SPAN_CENTI_C \
    (APP_AUTO_TEMP_MAX_CENTI_C - APP_AUTO_TEMP_MIN_CENTI_C)

#define RPM_SPAN \
    (APP_AUTO_FAN_MAX_RPM - APP_AUTO_FAN_MIN_RPM)

#define DAMPER_SPAN \
    (APP_AUTO_DAMPER_COLD_STEPS - APP_AUTO_DAMPER_HOT_STEPS)

static AppAutoControlSnapshot snap;
static AppAutoMode mode;
static AppAutoState state;
static AppAutoFanCtrlState fan_ctrl_state;
static AppAutoDamperCtrlState damper_ctrl_state;

static uint32_t last_process_tick;
static uint32_t auto_start_tick;
static uint32_t update_seq;

static uint32_t last_fan_adj_tick;
static uint32_t fan_adj_count;
static uint32_t fan_fault_count;

static uint16_t auto_fan_duty;

static uint32_t last_damper_act_tick;
static uint32_t damper_cmd_count;
static uint32_t damper_busy_count;
static uint32_t damper_fault_count;
static bool auto_damper_owned;
static bool failsafe_stop_issued;
static bool damper_pos_invalid;

static bool init_ok;

__attribute__((unused))
static bool config_check(void)
{
    if (APP_AUTO_TEMP_MAX_CENTI_C <= APP_AUTO_TEMP_MIN_CENTI_C)
        return false;
    if (APP_AUTO_FAN_MAX_RPM < APP_AUTO_FAN_MIN_RPM)
        return false;
    if (APP_AUTO_DAMPER_COLD_STEPS < 0 ||
        APP_AUTO_DAMPER_COLD_STEPS > 1700)
        return false;
    if (APP_AUTO_DAMPER_HOT_STEPS < 0 ||
        APP_AUTO_DAMPER_HOT_STEPS > 1700)
        return false;
    if (APP_AUTO_DAMPER_COLD_STEPS <= APP_AUTO_DAMPER_HOT_STEPS)
        return false;
    if (APP_AUTO_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_TEMP_STARTUP_GRACE_MS == 0U)
        return false;
    if (APP_AUTO_FAN_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_FAN_TOLERANCE_RPM == 0U)
        return false;
    if (APP_AUTO_FAN_PWM_STEP_X100 == 0U)
        return false;
    if (APP_AUTO_FAN_INITIAL_DUTY_X100 < APP_FAN_MIN_DUTY_X100 ||
        APP_AUTO_FAN_INITIAL_DUTY_X100 > APP_FAN_MAX_DUTY_X100)
        return false;
    if (APP_AUTO_FAN_FAILSAFE_DUTY_X100 < APP_FAN_MIN_DUTY_X100 ||
        APP_AUTO_FAN_FAILSAFE_DUTY_X100 > APP_FAN_MAX_DUTY_X100)
        return false;
    if (APP_AUTO_FAN_FAILSAFE_DUTY_X100 < APP_AUTO_FAN_INITIAL_DUTY_X100)
        return false;
    if (APP_AUTO_DAMPER_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_DAMPER_DEADBAND_STEPS == 0U ||
        APP_AUTO_DAMPER_DEADBAND_STEPS > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return false;
    if (APP_AUTO_DAMPER_FAILSAFE_STEPS < 0 ||
        (uint32_t)APP_AUTO_DAMPER_FAILSAFE_STEPS > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return false;
    return true;
}

static void evaluate_targets(int16_t control_temp)
{
    int16_t clamped = control_temp;
    uint32_t rpm, damper;

    if (clamped < APP_AUTO_TEMP_MIN_CENTI_C)
        clamped = APP_AUTO_TEMP_MIN_CENTI_C;
    if (clamped > APP_AUTO_TEMP_MAX_CENTI_C)
        clamped = APP_AUTO_TEMP_MAX_CENTI_C;

    {
        int32_t temp_delta = (int32_t)clamped - APP_AUTO_TEMP_MIN_CENTI_C;

        int64_t rpm_num = (int64_t)temp_delta * (int64_t)RPM_SPAN
                        + (int64_t)(TEMP_SPAN_CENTI_C / 2);
        rpm = APP_AUTO_FAN_MIN_RPM
            + (uint32_t)(rpm_num / (int64_t)TEMP_SPAN_CENTI_C);

        int64_t damper_num = (int64_t)temp_delta * (int64_t)DAMPER_SPAN
                           + (int64_t)(TEMP_SPAN_CENTI_C / 2);
        damper = (uint32_t)APP_AUTO_DAMPER_COLD_STEPS
               - (uint32_t)(damper_num / (int64_t)TEMP_SPAN_CENTI_C);
    }

    if (rpm < APP_AUTO_FAN_MIN_RPM) rpm = APP_AUTO_FAN_MIN_RPM;
    if (rpm > APP_AUTO_FAN_MAX_RPM) rpm = APP_AUTO_FAN_MAX_RPM;
    if (damper > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        damper = (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS;

    snap.target_fan_rpm     = (uint16_t)rpm;
    snap.target_damper_steps = (int32_t)damper;
}

__attribute__((unused))
static void do_evaluation(void)
{
    AppNtcSnapshot ntc;
    bool temp_valid;

    if (!AppNtc_GetSnapshot(&ntc))
    {
        snap.flags &= ~0x01U;
        return;
    }

    snap.ntc_state        = (uint8_t)ntc.state;
    snap.ntc_range_status = (uint8_t)ntc.range_status;
    snap.measured_temp_centi_c  = ntc.temp_centi_c;
    snap.control_temp_centi_c   = ntc.control_temp_centi_c;

    temp_valid = ntc.sensor_measurement_valid;

    {
        uint8_t f = snap.flags & 0xC0U;

        if (temp_valid) f |= 0x01U;
        if (ntc.range_status == APP_NTC_RANGE_CLAMPED_LOW)  f |= 0x02U;
        if (ntc.range_status == APP_NTC_RANGE_CLAMPED_HIGH) f |= 0x04U;

        snap.flags = f;
    }

    if (!temp_valid)
    {
        uint32_t now = HAL_GetTick();

        if ((uint32_t)(now - auto_start_tick)
            < APP_AUTO_TEMP_STARTUP_GRACE_MS)
        {
            state = APP_AUTO_STATE_WAIT_TEMP;
            snap.flags &= ~0x08U;
        }
        else
        {
            state = APP_AUTO_STATE_TEMP_FAULT;
            snap.target_fan_rpm      = APP_AUTO_FAN_MAX_RPM;
            snap.target_damper_steps  = APP_AUTO_DAMPER_HOT_STEPS;
            snap.flags |= 0x18U;
        }
    }
    else
    {
        state = APP_AUTO_STATE_TARGET_READY;
        snap.flags |= 0x08U;
        snap.flags &= ~0x10U;

        evaluate_targets(ntc.control_temp_centi_c);
    }

    update_seq++;
    snap.update_seq      = update_seq;
    snap.last_update_tick = HAL_GetTick();
    snap.state            = (uint8_t)state;
}

__attribute__((unused))
static bool targets_are_valid(void)
{
    return (snap.flags & 0x08U) != 0U;
}

static void fan_set_duty(uint16_t new_duty)
{
    if (new_duty < APP_FAN_MIN_DUTY_X100)
        new_duty = APP_FAN_MIN_DUTY_X100;
    if (new_duty > APP_FAN_MAX_DUTY_X100)
        new_duty = APP_FAN_MAX_DUTY_X100;

    if (new_duty == auto_fan_duty) return;

    if (AppFan_SetEnabled(true, new_duty))
    {
        auto_fan_duty = new_duty;
        snap.fan_control_result = 0U;
    }
    else
    {
        fan_ctrl_state = APP_AUTO_FAN_CTRL_HW_ERROR;
        snap.fan_control_result = 1U;
        snap.flags |= 0x80U;
    }
}

__attribute__((unused))
static void fan_control_process(void)
{
    AppFanSnapshot fan_snap;
    uint16_t current_duty;
    bool fan_enabled;
    bool tach_ok;
    uint32_t now;

    if (!AppFan_GetSnapshot(&fan_snap)) return;

    now = HAL_GetTick();
    fan_enabled = fan_snap.enabled;
    current_duty = fan_snap.applied_duty_x100;
    tach_ok = (fan_snap.tach_valid != 0U);

    snap.fan_state       = (uint8_t)fan_snap.state;
    snap.fan_tach_valid  = tach_ok ? 1U : 0U;
    snap.actual_fan_rpm  = fan_snap.rpm;
    snap.applied_fan_duty_x100 = current_duty;

    if (current_duty > 0U && fan_enabled)
    {
        auto_fan_duty = current_duty;
    }

    if (!fan_enabled)
    {
        fan_set_duty(APP_AUTO_FAN_INITIAL_DUTY_X100);
        fan_ctrl_state = APP_AUTO_FAN_CTRL_STARTING;
        last_fan_adj_tick = now;
        return;
    }

    if (fan_snap.state == APP_FAN_STATE_STARTUP_BOOST)
    {
        fan_ctrl_state = APP_AUTO_FAN_CTRL_STARTING;
        auto_fan_duty = current_duty;
        last_fan_adj_tick = now;
        return;
    }

    if (current_duty == 0U)
    {
        fan_ctrl_state = APP_AUTO_FAN_CTRL_INACTIVE;
        return;
    }

    if (fan_snap.state == APP_FAN_STATE_NO_TACH)
    {
        if (current_duty != APP_AUTO_FAN_FAILSAFE_DUTY_X100)
        {
            fan_set_duty(APP_AUTO_FAN_FAILSAFE_DUTY_X100);
            fan_fault_count++;
        }
        fan_ctrl_state = APP_AUTO_FAN_CTRL_TACH_FAULT;
        snap.fan_fault_count = fan_fault_count;
        last_fan_adj_tick = now;
        return;
    }

    if (fan_ctrl_state == APP_AUTO_FAN_CTRL_TACH_FAULT)
    {
        if (tach_ok)
            snap.flags &= ~0x80U;
        else
        {
            last_fan_adj_tick = now;
            return;
        }
    }

    if (!tach_ok)
    {
        fan_ctrl_state = APP_AUTO_FAN_CTRL_WAIT_TACH;
        last_fan_adj_tick = now;
        return;
    }

    if ((uint32_t)(now - last_fan_adj_tick) < APP_AUTO_FAN_CONTROL_PERIOD_MS)
        return;

    {
        int32_t error = (int32_t)snap.target_fan_rpm - (int32_t)fan_snap.rpm;
        snap.fan_error_rpm = (int16_t)error;

        int32_t tol = (int32_t)APP_AUTO_FAN_TOLERANCE_RPM;

        if (error > tol)
        {
            if (current_duty >= APP_FAN_MAX_DUTY_X100)
            {
                fan_ctrl_state = APP_AUTO_FAN_CTRL_SATURATED_HIGH;
                last_fan_adj_tick = now;
                return;
            }

            fan_set_duty(current_duty + APP_AUTO_FAN_PWM_STEP_X100);
            fan_ctrl_state = APP_AUTO_FAN_CTRL_ADJUSTING;
            fan_adj_count++;
            last_fan_adj_tick = now;
        }
        else if (error < -tol)
        {
            uint16_t new_duty;

            if (current_duty <= APP_FAN_MIN_DUTY_X100)
            {
                fan_ctrl_state = APP_AUTO_FAN_CTRL_SATURATED_LOW;
                last_fan_adj_tick = now;
                return;
            }

            if (current_duty < APP_AUTO_FAN_PWM_STEP_X100)
                new_duty = APP_FAN_MIN_DUTY_X100;
            else
                new_duty = current_duty - APP_AUTO_FAN_PWM_STEP_X100;

            fan_set_duty(new_duty);
            fan_ctrl_state = APP_AUTO_FAN_CTRL_ADJUSTING;
            fan_adj_count++;
            last_fan_adj_tick = now;
        }
        else
        {
            fan_ctrl_state = APP_AUTO_FAN_CTRL_IN_TOLERANCE;
        }
    }

    snap.fan_adjust_count    = fan_adj_count;
    snap.fan_fault_count     = fan_fault_count;
    snap.last_fan_control_tick = last_fan_adj_tick;
    snap.fan_control_state   = (uint8_t)fan_ctrl_state;

    if (fan_ctrl_state == APP_AUTO_FAN_CTRL_IN_TOLERANCE)
        snap.flags |= 0x40U;
    else
        snap.flags &= ~0x40U;
}

__attribute__((unused))
static void damper_control_process(void)
{
    DamperSnapshot damper_snap;
    uint32_t now;
    bool pos_valid;
    int32_t damper_state_en;
    int32_t target;

    if (!AppDamper_GetSnapshot(&damper_snap)) return;

    now = HAL_GetTick();
    pos_valid = (damper_snap.flags & 0x01U) != 0U;
    damper_state_en = (int32_t)damper_snap.damper_state;
    target = snap.target_damper_steps;

    snap.damper_state           = damper_snap.damper_state;
    snap.damper_position_valid  = pos_valid ? 1U : 0U;
    snap.actual_damper_steps    = damper_snap.current_steps;
    snap.requested_damper_target_steps = target;

    if (!pos_valid)
    {
        auto_damper_owned = false;
        damper_pos_invalid = true;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_POSITION;
        return;
    }

    damper_pos_invalid = false;

    /* FAULT state */
    if (damper_state_en == DAMPER_STATE_FAULT)
    {
        auto_damper_owned = false;
        failsafe_stop_issued = false;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAULT;
        damper_fault_count++;
        return;
    }

    /* Moving or POST_MOVE_HOLD */
    if (damper_state_en == DAMPER_STATE_MOVING_FORWARD ||
        damper_state_en == DAMPER_STATE_MOVING_REVERSE)
    {
        if (auto_damper_owned)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_MOVING;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        }
        return;
    }

    if (damper_state_en == DAMPER_STATE_POST_MOVE_HOLD)
    {
        if (auto_damper_owned)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_POST_HOLD;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        }
        return;
    }

    /* Movement finished */
    if (auto_damper_owned &&
        (damper_state_en == DAMPER_STATE_IDLE_RELEASED ||
         damper_state_en == DAMPER_STATE_STOPPED))
    {
        auto_damper_owned = false;
    }

    /* Failsafe: temp fault → target = 0 */
    if ((snap.flags & 0x10U) != 0U)
    {
        if (!auto_damper_owned)
        {
            if (failsafe_stop_issued)
            {
                if (damper_state_en == DAMPER_STATE_STOPPED ||
                    damper_state_en == DAMPER_STATE_IDLE_RELEASED)
                {
                    failsafe_stop_issued = false;
                }
                else
                {
                    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAILSAFE_STOPPING;
                    return;
                }
            }

            {
                int64_t diff64 = 0LL - (int64_t)damper_snap.current_steps;
                uint32_t ad = (diff64 < 0) ? (uint32_t)(-diff64) : (uint32_t)diff64;

                if (ad < APP_AUTO_DAMPER_DEADBAND_STEPS)
                {
                    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_IN_DEADBAND;
                    return;
                }
            }

            if (target != 0)
            {
                damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
                return;
            }
        }
        else
        {
            if (target != 0)
            {
                AppDamper_Stop();
                auto_damper_owned = false;
                failsafe_stop_issued = true;
                damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAILSAFE_STOPPING;
                return;
            }
        }
    }
    else
    {
        failsafe_stop_issued = false;
    }

    /* Not idle */
    if (damper_state_en != DAMPER_STATE_IDLE_RELEASED &&
        damper_state_en != DAMPER_STATE_STOPPED)
    {
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        return;
    }

    /* Idle - evaluate deadband */
    if ((uint32_t)(now - last_damper_act_tick) < APP_AUTO_DAMPER_CONTROL_PERIOD_MS)
        return;

    {
        int64_t diff64 = (int64_t)target - (int64_t)damper_snap.current_steps;
        uint32_t ad = (diff64 < 0) ? (uint32_t)(-diff64) : (uint32_t)diff64;

        if (ad < APP_AUTO_DAMPER_DEADBAND_STEPS)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_IN_DEADBAND;
            return;
        }
    }

    if (target < 0)
        target = 0;
    if (target > (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        target = (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS;

    snap.last_commanded_damper_steps = target;
    snap.damper_error_steps = target - damper_snap.current_steps;

    {
        uint8_t result = AppDamper_MoveAbsolute(target);
        snap.damper_control_result = result;

        if (result == DAMPER_STATUS_OK)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_COMMAND_SUBMITTED;
            auto_damper_owned = true;
            damper_cmd_count++;
            last_damper_act_tick = now;
        }
        else if (result == DAMPER_STATUS_BUSY)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
            damper_busy_count++;
            last_damper_act_tick = now;
        }
        else if (result == DAMPER_STATUS_NO_VALID_DATA)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_POSITION;
            auto_damper_owned = false;
        }
        else if (result == DAMPER_STATUS_PARAM_RANGE)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_COMMAND_ERROR;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAULT;
            damper_fault_count++;
            auto_damper_owned = false;
        }
    }

    snap.damper_command_count = damper_cmd_count;
    snap.damper_busy_count    = damper_busy_count;
    snap.damper_fault_count   = damper_fault_count;
    snap.last_damper_control_tick = last_damper_act_tick;
    snap.damper_auto_owned    = auto_damper_owned ? 1U : 0U;
    snap.damper_failsafe_stop_issued = failsafe_stop_issued ? 1U : 0U;
    snap.damper_control_state = (uint8_t)damper_ctrl_state;
}

void AppAutoControl_Init(void)
{
#if APP_AUTO_CONTROL_ENABLED
    mode = APP_AUTO_MODE_MANUAL;

    if (!config_check())
    {
        state  = APP_AUTO_STATE_CONFIG_ERROR;
        init_ok = false;
        return;
    }

    state = APP_AUTO_STATE_UNINITIALIZED;
    init_ok = true;

    fan_ctrl_state    = APP_AUTO_FAN_CTRL_INACTIVE;
    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
#else
    mode   = APP_AUTO_MODE_MANUAL;
    state  = APP_AUTO_STATE_UNAVAILABLE;
    init_ok = false;

    fan_ctrl_state    = APP_AUTO_FAN_CTRL_UNAVAILABLE;
    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_UNAVAILABLE;
#endif

    auto_fan_duty     = 0U;
    last_fan_adj_tick = 0U;
    fan_adj_count     = 0U;
    fan_fault_count   = 0U;

    damper_ctrl_state      = APP_AUTO_DAMPER_CTRL_INACTIVE;
    last_damper_act_tick   = 0U;
    damper_cmd_count       = 0U;
    damper_busy_count      = 0U;
    damper_fault_count     = 0U;
    auto_damper_owned      = false;
    failsafe_stop_issued   = false;
    damper_pos_invalid     = false;

    snap.mode                  = (uint8_t)mode;
    snap.state                 = (uint8_t)state;
    snap.flags                 = 0x00U;
    snap.ntc_state             = 0U;
    snap.ntc_range_status      = 0U;
    snap.measured_temp_centi_c = 0;
    snap.control_temp_centi_c  = 0;
    snap.target_fan_rpm        = 0U;
    snap.target_damper_steps   = 0;
    snap.update_seq            = 0U;
    snap.last_update_tick      = 0U;

    snap.fan_control_state    = (uint8_t)fan_ctrl_state;
    snap.fan_state            = 0U;
    snap.fan_tach_valid       = 0U;
    snap.fan_control_result   = 0U;
    snap.actual_fan_rpm       = 0U;
    snap.applied_fan_duty_x100 = 0U;
    snap.fan_error_rpm        = 0;
    snap.last_fan_control_tick = 0U;
    snap.fan_adjust_count     = 0U;
    snap.fan_fault_count      = 0U;

    snap.damper_control_state        = (uint8_t)damper_ctrl_state;
    snap.damper_state                = 0U;
    snap.damper_position_valid       = 0U;
    snap.damper_control_result       = 0U;
    snap.damper_auto_owned           = 0U;
    snap.damper_failsafe_stop_issued = 0U;
    snap.reserved_damper             = 0U;
    snap.actual_damper_steps         = 0;
    snap.requested_damper_target_steps = 0;
    snap.last_commanded_damper_steps = 0;
    snap.damper_error_steps          = 0;
    snap.last_damper_control_tick    = 0U;
    snap.damper_command_count        = 0U;
    snap.damper_busy_count           = 0U;
    snap.damper_fault_count          = 0U;

    last_process_tick = 0U;
    auto_start_tick   = 0U;
    update_seq        = 0U;
}

uint8_t AppAutoControl_SetMode(uint8_t new_mode)
{
#if APP_AUTO_CONTROL_ENABLED
    if (!init_ok)
        return APP_AUTO_SET_MODE_CONFIG_ERROR;

    if (state == APP_AUTO_STATE_CONFIG_ERROR)
        return APP_AUTO_SET_MODE_CONFIG_ERROR;

    if (new_mode != APP_AUTO_MODE_MANUAL && new_mode != APP_AUTO_MODE_AUTO)
        return APP_AUTO_SET_MODE_INVALID_PARAM;

    if ((AppAutoMode)new_mode == mode)
        return APP_AUTO_SET_MODE_OK;

    if (new_mode == APP_AUTO_MODE_MANUAL && mode == APP_AUTO_MODE_AUTO
        && APP_AUTO_DAMPER_STOP_ON_MANUAL && auto_damper_owned)
    {
        AppDamper_Stop();
        auto_damper_owned = false;
        failsafe_stop_issued = false;
    }

    mode = (AppAutoMode)new_mode;
    snap.mode = (uint8_t)mode;

    if (mode == APP_AUTO_MODE_AUTO)
    {
        auto_start_tick = HAL_GetTick();
        fan_ctrl_state = APP_AUTO_FAN_CTRL_INACTIVE;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        auto_damper_owned = false;
        failsafe_stop_issued = false;
        damper_pos_invalid = false;
        do_evaluation();
    }
    else
    {
        state = APP_AUTO_STATE_MANUAL;
        fan_ctrl_state = APP_AUTO_FAN_CTRL_INACTIVE;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        last_process_tick = 0U;
        snap.state = (uint8_t)state;
    }

    return APP_AUTO_SET_MODE_OK;
#else
    (void)new_mode;
    return APP_AUTO_SET_MODE_UNAVAILABLE;
#endif
}

void AppAutoControl_Process(void)
{
    if (!init_ok) return;

#if APP_AUTO_CONTROL_ENABLED
    if (state == APP_AUTO_STATE_UNINITIALIZED)
    {
        if (mode == APP_AUTO_MODE_AUTO)
        {
            auto_start_tick = HAL_GetTick();
            do_evaluation();
        }
        else
        {
            state = APP_AUTO_STATE_MANUAL;
            snap.state = (uint8_t)state;
        }
        return;
    }

    if (mode == APP_AUTO_MODE_MANUAL)
    {
        state = APP_AUTO_STATE_MANUAL;
        fan_ctrl_state = APP_AUTO_FAN_CTRL_INACTIVE;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        snap.fan_control_state = (uint8_t)fan_ctrl_state;
        snap.damper_control_state = (uint8_t)damper_ctrl_state;
        snap.state = (uint8_t)state;
        return;
    }

    {
        uint32_t now = HAL_GetTick();

        if ((uint32_t)(now - last_process_tick) >= APP_AUTO_CONTROL_PERIOD_MS)
        {
            last_process_tick = now;
            do_evaluation();
        }
    }

    if (targets_are_valid())
    {
        fan_control_process();
        damper_control_process();
    }
    else
    {
        fan_ctrl_state = APP_AUTO_FAN_CTRL_INACTIVE;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
    }

    snap.fan_control_state = (uint8_t)fan_ctrl_state;
    snap.damper_control_state = (uint8_t)damper_ctrl_state;
    snap.state = (uint8_t)state;
#endif
}

bool AppAutoControl_GetSnapshot(AppAutoControlSnapshot *s)
{
    if (s == NULL) return false;
    *s = snap;
    return true;
}

uint8_t AppAutoControl_GetMode(void)
{
    return (uint8_t)mode;
}
