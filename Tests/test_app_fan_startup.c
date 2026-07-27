#include "app_fan.h"
#include "app_fan_config.h"
#include "app_fan_feedback_adc.h"
#include "tim.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static TIM_TypeDef tim10_instance;
TIM_HandleTypeDef htim10 = { &tim10_instance };

static uint32_t fake_tick;
static AppFanFeedbackSnapshot fake_feedback;
static uint32_t reset_count;
static uint32_t reacquire_count;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *handle, uint32_t channel)
{
    (void)handle;
    (void)channel;
    return HAL_OK;
}

void AppFanFeedback_ResetMeasurement(void)
{
    reset_count++;
    fake_feedback.state = 0U;
    fake_feedback.freq_millihz = 0U;
    fake_feedback.update_seq++;
}

void AppFanFeedback_ReacquireAfterDutyChange(void)
{
    reacquire_count++;
}

bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = fake_feedback;
    return true;
}

static uint16_t applied_duty_from_ccr(void)
{
    return (uint16_t)(tim10_instance.CCR1 * 100U);
}

int main(void)
{
    AppFanSnapshot snapshot;

    memset(&fake_feedback, 0, sizeof(fake_feedback));
    fake_tick = 0U;
    reset_count = 0U;
    reacquire_count = 0U;

    assert(APP_FAN_STARTUP_BOOST_MS == 5000U);
    assert(AppFan_Init());
    assert(AppFan_SetEnabled(true, 2300U));
    assert(reset_count == 1U);
    assert(applied_duty_from_ccr() == 10000U);

    assert(AppFan_GetSnapshot(&snapshot));
    assert(snapshot.state == APP_FAN_STATE_STARTUP_BOOST);
    assert(snapshot.target_duty_x100 == 2300U);
    assert(snapshot.applied_duty_x100 == 10000U);

    fake_feedback.state = 1U;
    fake_feedback.freq_millihz = 76667U;
    fake_feedback.update_seq += 10U;

    fake_tick = 4999U;
    AppFan_Process();
    assert(applied_duty_from_ccr() == 10000U);

    fake_tick = 5000U;
    AppFan_Process();
    assert(applied_duty_from_ccr() == 2300U);
    assert(reacquire_count == 1U);
    assert(AppFan_GetSnapshot(&snapshot));
    assert(snapshot.state == APP_FAN_STATE_TACH_UNRELIABLE);
    assert(snapshot.tach_valid == 0U);

    /* The last measurement from the 100% boost is synchronized away. */
    fake_tick = 5001U;
    AppFan_Process();
    assert(AppFan_GetSnapshot(&snapshot));
    assert(snapshot.tach_valid == 0U);

    /* Only a fresh post-transition feedback sequence may become valid. */
    fake_feedback.freq_millihz = 50000U;
    fake_feedback.update_seq++;
    fake_tick = 5002U;
    AppFan_Process();
    assert(AppFan_GetSnapshot(&snapshot));
    assert(snapshot.tach_valid == 1U);
    assert(snapshot.rpm == 1500U);
    assert(snapshot.state == APP_FAN_STATE_RUNNING);

    puts("fan startup boost tests passed");
    return 0;
}
