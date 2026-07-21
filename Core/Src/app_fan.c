#include "app_fan.h"
#include "app_fan_config.h"
#include "app_fan_feedback_adc.h"
#include "tim.h"
#include <stddef.h>

static AppFanState state;
static bool     enabled;
static uint16_t target_duty;
static uint16_t applied_duty;
static uint32_t boost_start_tick;
static uint32_t last_fg_fmhz;
static uint16_t last_rpm;
static uint32_t last_tach_tick;
static bool     init_ok;

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

bool AppFan_Init(void)
{
    state       = APP_FAN_STATE_OFF;
    enabled     = false;
    target_duty = APP_FAN_MAX_DUTY_X100;
    applied_duty = 0U;
    last_fg_fmhz = 0U;
    last_rpm     = 0U;
    init_ok = false;

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
            state = (target_duty >= APP_FAN_MAX_DUTY_X100)
                    ? APP_FAN_STATE_RUNNING
                    : APP_FAN_STATE_TACH_UNRELIABLE;
        }
        return;
    }

    if (!enabled) return;

    AppFanFeedbackSnapshot fs;
    if (AppFanFeedback_GetSnapshot(&fs) && (fs.state == 1U))
    {
        last_fg_fmhz   = fs.freq_millihz;
        last_rpm       = calc_rpm(fs.freq_millihz);
        last_tach_tick = HAL_GetTick();

        if (applied_duty >= APP_FAN_MAX_DUTY_X100)
        {
            uint32_t age = HAL_GetTick() - last_tach_tick;
            state = (age < APP_FAN_NO_TACH_TIMEOUT_MS)
                    ? APP_FAN_STATE_RUNNING
                    : APP_FAN_STATE_NO_TACH;
        }
    }
    else
    {
        if ((applied_duty >= APP_FAN_MAX_DUTY_X100) &&
            ((HAL_GetTick() - last_tach_tick) >= APP_FAN_NO_TACH_TIMEOUT_MS))
        {
            state = APP_FAN_STATE_NO_TACH;
            last_fg_fmhz = 0U;
            last_rpm     = 0U;
        }
    }
}

bool AppFan_SetEnabled(bool en, uint16_t duty)
{
    if (!init_ok) return false;

    if (!en)
    {
        if (duty != 0U) return false;
        if (!apply_duty(0U)) { state = APP_FAN_STATE_PWM_ERROR; enabled = false; return false; }
        applied_duty = 0U;
        enabled     = false;
        state       = APP_FAN_STATE_OFF;
        return true;
    }

    if ((duty < APP_FAN_MIN_DUTY_X100) || (duty > APP_FAN_MAX_DUTY_X100))
        return false;

    target_duty = duty;

    if (!apply_duty(APP_FAN_MAX_DUTY_X100))
    {
        apply_duty(0U);
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }

    applied_duty     = APP_FAN_MAX_DUTY_X100;
    enabled          = true;
    boost_start_tick = HAL_GetTick();
    state            = APP_FAN_STATE_STARTUP_BOOST;
    return true;
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

    uint32_t age = (state == APP_FAN_STATE_OFF)
                   ? 65535U
                   : HAL_GetTick() - last_tach_tick;
    s->tach_age_ms = (age > 65535U) ? 65535U : (uint16_t)age;

    return true;
}
