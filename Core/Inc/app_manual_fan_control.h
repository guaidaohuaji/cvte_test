#ifndef APP_MANUAL_FAN_CONTROL_H
#define APP_MANUAL_FAN_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_MANUAL_FAN_MODE_OFF   = 0,
    APP_MANUAL_FAN_MODE_DUTY  = 1,
    APP_MANUAL_FAN_MODE_SPEED = 2
} AppManualFanMode;

typedef enum {
    APP_MANUAL_FAN_CTRL_INACTIVE       = 0,
    APP_MANUAL_FAN_CTRL_STARTING       = 1,
    APP_MANUAL_FAN_CTRL_WAIT_TACH      = 2,
    APP_MANUAL_FAN_CTRL_ADJUSTING      = 3,
    APP_MANUAL_FAN_CTRL_IN_TOLERANCE   = 4,
    APP_MANUAL_FAN_CTRL_TACH_FAULT     = 5,
    APP_MANUAL_FAN_CTRL_SATURATED_LOW  = 6,
    APP_MANUAL_FAN_CTRL_SATURATED_HIGH = 7,
    APP_MANUAL_FAN_CTRL_HW_ERROR       = 8
} AppManualFanCtrlState;

typedef enum {
    APP_MANUAL_FAN_RESULT_OK = 0,
    APP_MANUAL_FAN_RESULT_INVALID_PARAM,
    APP_MANUAL_FAN_RESULT_MODE_LOCKED,
    APP_MANUAL_FAN_RESULT_HW_ERROR
} AppManualFanResult;

typedef struct {
    uint8_t  mode;
    uint8_t  control_state;
    uint16_t target_rpm;
    int16_t  rpm_error;
    uint8_t  tach_valid;
    uint8_t  in_tolerance;
    uint16_t feedforward_duty_x100;
    uint32_t adjust_count;
    uint32_t fault_count;
} AppManualFanControlSnapshot;

void AppManualFanControl_Init(void);
void AppManualFanControl_Process(void);
AppManualFanResult AppManualFanControl_SetOff(void);
AppManualFanResult AppManualFanControl_SetDuty(uint16_t duty_x100);
AppManualFanResult AppManualFanControl_SetTargetRpm(uint16_t target_rpm);
bool AppManualFanControl_GetSnapshot(AppManualFanControlSnapshot *snapshot);

#endif
