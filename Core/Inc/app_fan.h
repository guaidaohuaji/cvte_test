#ifndef APP_FAN_H
#define APP_FAN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_FAN_STATE_OFF = 0,
    APP_FAN_STATE_STARTUP_BOOST = 1,
    APP_FAN_STATE_RUNNING = 2,
    APP_FAN_STATE_NO_TACH = 3,
    APP_FAN_STATE_TACH_UNRELIABLE = 4,
    APP_FAN_STATE_PWM_ERROR = 5,
    APP_FAN_STATE_CONFIG_ERROR = 6
} AppFanState;

typedef struct {
    AppFanState state;
    bool        enabled;
    uint16_t    target_duty_x100;
    uint16_t    applied_duty_x100;
    uint16_t    pwm_frequency_hz;
    uint32_t    fg_frequency_millihz;
    uint16_t    rpm;
    uint16_t    tach_age_ms;
} AppFanSnapshot;

bool AppFan_Init(void);
void AppFan_Process(void);
bool AppFan_SetEnabled(bool enabled, uint16_t duty_x100);
bool AppFan_GetSnapshot(AppFanSnapshot *snap);

#endif
