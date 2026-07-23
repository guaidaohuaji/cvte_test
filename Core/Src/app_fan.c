#include "app_fan.h"
#include "app_fan_config.h"
#include "app_fan_feedback_adc.h"
#include "tim.h"
#include <stddef.h>

#define APP_FAN_TACH_REACQUIRE_GRACE_MS  400U
#define APP_FAN_PERSISTENT_NO_TACH_MS   1000U

static AppFanState state;
static bool     enabled;
static uint16_t target_duty;
static uint16_t applied_duty;
static uint32_t boost_start_tick;
static uint32_t last_fg_fmhz;
static uint16_t last_rpm;
static uint32_t last_tach_tick;
static uint32_t last_fg_update_seq;
static uint32_t fan_start_tick;
static bool     init_ok;
static bool     tach_valid;
static bool     ever_had_valid_tach;
static uint32_t last_duty_change_tick;

static bool apply_duty(uint16_t d)
{
    uint64_t cmp = ((uint64_t)(APP_FAN_PWM_PERIOD + 1U) * (uint64_t)d + 5000ULL) / 10000ULL;
    if (cmp > 65535ULL) cmp = 65535ULL;
    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, (uint32_t)cmp);
    uint32_t rd = htim10.Instance->CCR1;
    return (rd == (uint32_t)cmp);
}

static uint16_t calc_rpm(uint32_t fmhz)
{
    uint64_t r = ((uint64_t)fmhz * APP_FAN_RPM_FACTOR + 500ULL) / 1000ULL;
    return (r > 65535ULL) ? 65535U : (uint16_t)r;
}

static bool is_grace_period_active(void)
{
    return (HAL_GetTick() - fan_start_tick) < APP_FAN_STARTUP_BOOST_MS;
}

static bool is_reacquire_grace_active(uint32_t now)
{
    return ((now - last_duty_change_tick) < APP_FAN_TACH_REACQUIRE_GRACE_MS);
}

bool AppFan_Init(void)
{
    state       = APP_FAN_STATE_OFF;
    enabled     = false;
    target_duty = APP_FAN_MAX_DUTY_X100;
    applied_duty = 0U;
    last_fg_fmhz = 0U;
    last_rpm     = 0U;
    last_tach_tick = 0U;
    last_fg_update_seq = 0U;
    fan_start_tick     = 0U;
    last_duty_change_tick = 0U;
    init_ok = false;
    tach_valid = false;
    ever_had_valid_tach = false;

    if ((APP_FAN_PWM_PRESCALER > 65535U) ||
        (APP_FAN_PWM_PERIOD > 65535U) ||
        (APP_FAN_PWM_PERIOD == 0U) ||
        (APP_FAN_MIN_DUTY_X100 > APP_FAN_MAX_DUTY_X100))
    {
        state = APP_FAN_STATE_CONFIG_ERROR;
        return false;
    }

    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, 0U);

    if (HAL_TIM_PWM_Start(&htim10, TIM_CHANNEL_1) != HAL_OK)
    {
        state = APP_FAN_STATE_PWM_ERROR;
        return false;
    }

    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, 0U);
    init_ok = true;
    return true;
}

static void handle_disable(void)
{
    if (!apply_duty(0U)) { state = APP_FAN_STATE_PWM_ERROR; enabled = false; return; }
    applied_duty = 0U;
    enabled     = false;
    state       = APP_FAN_STATE_OFF;
    last_fg_fmhz = 0U;
    last_rpm     = 0U;
    last_tach_tick = 0U;
    tach_valid = false;
    ever_had_valid_tach = false;
}

static bool handle_fresh_enable(uint16_t duty)
{
    if (!apply_duty(APP_FAN_MAX_DUTY_X100))
    {
        apply_duty(0U);
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }

    AppFanFeedback_ResetMeasurement();

    AppFanFeedbackSnapshot tmp;
    if (AppFanFeedback_GetSnapshot(&tmp))
        last_fg_update_seq = tmp.update_seq;
    else
        last_fg_update_seq = 0U;

    applied_duty     = APP_FAN_MAX_DUTY_X100;
    enabled          = true;
    boost_start_tick = HAL_GetTick();
    state            = APP_FAN_STATE_STARTUP_BOOST;
    fan_start_tick   = HAL_GetTick();
    last_fg_fmhz     = 0U;
    last_rpm         = 0U;
    last_tach_tick   = 0U;
    tach_valid       = false;
    ever_had_valid_tach = false;
    last_duty_change_tick = HAL_GetTick();

    target_duty = duty;
    return true;
}

static bool handle_duty_update(uint16_t duty)
{
    if (target_duty == duty) return true;

    target_duty = duty;

    if (state == APP_FAN_STATE_STARTUP_BOOST)
        return true;

    if (!apply_duty(duty))
    {
        apply_duty(0U);
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }
    applied_duty = duty;
    AppFanFeedback_ResetMeasurement();
    tach_valid = false;
    last_duty_change_tick = HAL_GetTick();
    state = APP_FAN_STATE_RUNNING;
    return true;
}

void AppFan_Process(void)
{
    if (!init_ok) return;

    if (state == APP_FAN_STATE_STARTUP_BOOST)
    {
        if ((HAL_GetTick() - boost_start_tick) >= APP_FAN_STARTUP_BOOST_MS)
        {
            if (!apply_duty(target_duty))
            {
                apply_duty(0U);
                state   = APP_FAN_STATE_PWM_ERROR;
                enabled = false;
                return;
            }
            applied_duty = target_duty;
            state = APP_FAN_STATE_TACH_UNRELIABLE;
            last_duty_change_tick = HAL_GetTick();
        }
        return;
    }

    if (!enabled) return;
    if (applied_duty == 0U) return;

    uint32_t now = HAL_GetTick();
    AppFanFeedbackSnapshot fs;

    if (AppFanFeedback_GetSnapshot(&fs) && (fs.state == 1U))
    {
        if (fs.update_seq != last_fg_update_seq)
        {
            last_fg_fmhz   = fs.freq_millihz;
            last_rpm       = calc_rpm(fs.freq_millihz);
            last_tach_tick = now;
            last_fg_update_seq = fs.update_seq;
            tach_valid = true;
            ever_had_valid_tach = true;

            state = APP_FAN_STATE_RUNNING;
        }
        else
        {
            if (ever_had_valid_tach &&
                (now - last_tach_tick) >= APP_FAN_PERSISTENT_NO_TACH_MS)
            {
                state = APP_FAN_STATE_NO_TACH;
                last_fg_fmhz = 0U;
                last_rpm     = 0U;
                tach_valid   = false;
            }
            else if (!ever_had_valid_tach &&
                     !is_reacquire_grace_active(now))
            {
                state = APP_FAN_STATE_NO_TACH;
                last_fg_fmhz = 0U;
                last_rpm     = 0U;
                tach_valid   = false;
            }
        }
    }
    else
    {
        tach_valid = false;

        if (ever_had_valid_tach &&
            (now - last_tach_tick) >= APP_FAN_PERSISTENT_NO_TACH_MS)
        {
            state = APP_FAN_STATE_NO_TACH;
            last_fg_fmhz = 0U;
            last_rpm     = 0U;
            tach_valid   = false;
        }
        else if (!ever_had_valid_tach &&
                 !is_reacquire_grace_active(now))
        {
            state = APP_FAN_STATE_NO_TACH;
            last_fg_fmhz = 0U;
            last_rpm     = 0U;
            tach_valid   = false;
        }
        else if (applied_duty > 0U && !is_grace_period_active())
        {
            state = APP_FAN_STATE_TACH_UNRELIABLE;
        }
    }
}

bool AppFan_SetEnabled(bool en, uint16_t duty)
{
    if (!init_ok) return false;

    if (!en)
    {
        if (duty != 0U) return false;
        handle_disable();
        return true;
    }

    if (duty == 0U)
    {
        if (!enabled)
        {
            AppFanFeedback_ResetMeasurement();
            AppFanFeedbackSnapshot tmp;
            if (AppFanFeedback_GetSnapshot(&tmp))
                last_fg_update_seq = tmp.update_seq;
            else
                last_fg_update_seq = 0U;
        }
        if (!apply_duty(0U)) { state = APP_FAN_STATE_PWM_ERROR; enabled = false; return false; }
        applied_duty     = 0U;
        enabled          = true;
        state            = APP_FAN_STATE_OFF;
        target_duty      = 0U;
        last_fg_fmhz     = 0U;
        last_rpm         = 0U;
        last_tach_tick   = 0U;
        tach_valid       = false;
        ever_had_valid_tach = false;
        return true;
    }

    if ((duty < APP_FAN_MIN_DUTY_X100) || (duty > APP_FAN_MAX_DUTY_X100))
        return false;

    if (!enabled)
        return handle_fresh_enable(duty);

    return handle_duty_update(duty);
}

bool AppFan_GetSnapshot(AppFanSnapshot *s)
{
    if (s == NULL) return false;

    s->state               = state;
    s->enabled             = enabled;
    s->target_duty_x100    = target_duty;
    s->applied_duty_x100   = applied_duty;
    s->pwm_frequency_hz    = APP_FAN_PWM_FREQUENCY_HZ;
    s->fg_frequency_millihz = last_fg_fmhz;
    s->rpm                 = last_rpm;
    s->tach_valid          = tach_valid ? 1U : 0U;

    uint32_t age = (state == APP_FAN_STATE_OFF || last_tach_tick == 0U)
                   ? 65535U
                   : HAL_GetTick() - last_tach_tick;
    s->tach_age_ms = (age > 65535U) ? 65535U : (uint16_t)age;

    return true;
}
