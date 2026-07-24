#ifndef APP_AUTO_CONTROL_H
#define APP_AUTO_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "app_auto_control_config.h"

typedef enum {
    APP_AUTO_MODE_MANUAL = 0,
    APP_AUTO_MODE_AUTO   = 1
} AppAutoMode;

typedef enum {
    APP_AUTO_STATE_UNINITIALIZED = 0,
    APP_AUTO_STATE_MANUAL        = 1,
    APP_AUTO_STATE_WAIT_TEMP     = 2,
    APP_AUTO_STATE_TARGET_READY  = 3,
    APP_AUTO_STATE_TEMP_FAULT    = 4,
    APP_AUTO_STATE_CONFIG_ERROR  = 5,
    APP_AUTO_STATE_UNAVAILABLE   = 0xFE
} AppAutoState;

typedef enum {
    APP_AUTO_SET_MODE_OK            = 0,
    APP_AUTO_SET_MODE_INVALID_PARAM = 1,
    APP_AUTO_SET_MODE_UNAVAILABLE   = 2,
    APP_AUTO_SET_MODE_CONFIG_ERROR  = 3
} AppAutoSetModeResult;

typedef enum {
    APP_AUTO_FAN_CTRL_INACTIVE       = 0,
    APP_AUTO_FAN_CTRL_STARTING       = 1,
    APP_AUTO_FAN_CTRL_WAIT_TACH      = 2,
    APP_AUTO_FAN_CTRL_ADJUSTING      = 3,
    APP_AUTO_FAN_CTRL_IN_TOLERANCE   = 4,
    APP_AUTO_FAN_CTRL_TACH_FAULT     = 5,
    APP_AUTO_FAN_CTRL_SATURATED_LOW  = 6,
    APP_AUTO_FAN_CTRL_SATURATED_HIGH = 7,
    APP_AUTO_FAN_CTRL_HW_ERROR       = 8,
    APP_AUTO_FAN_CTRL_UNAVAILABLE    = 0xFE
} AppAutoFanCtrlState;

typedef enum {
    APP_AUTO_DAMPER_CTRL_INACTIVE          = 0,
    APP_AUTO_DAMPER_CTRL_WAIT_POSITION     = 1,
    APP_AUTO_DAMPER_CTRL_WAIT_IDLE         = 2,
    APP_AUTO_DAMPER_CTRL_IN_DEADBAND       = 3,
    APP_AUTO_DAMPER_CTRL_COMMAND_SUBMITTED = 4,
    APP_AUTO_DAMPER_CTRL_MOVING            = 5,
    APP_AUTO_DAMPER_CTRL_POST_HOLD         = 6,
    APP_AUTO_DAMPER_CTRL_FAILSAFE_STOPPING = 7,
    APP_AUTO_DAMPER_CTRL_FAULT             = 8,
    APP_AUTO_DAMPER_CTRL_COMMAND_ERROR     = 9,
    APP_AUTO_DAMPER_CTRL_UNAVAILABLE       = 0xFE
} AppAutoDamperCtrlState;

typedef struct {
    uint8_t  mode;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  ntc_state;
    uint8_t  ntc_range_status;

    int16_t  measured_temp_centi_c;
    int16_t  control_temp_centi_c;

    uint16_t target_fan_rpm;
    int32_t  target_damper_steps;

    uint32_t update_seq;
    uint32_t last_update_tick;

    uint8_t  fan_control_state;
    uint8_t  fan_state;
    uint8_t  fan_tach_valid;
    uint8_t  fan_control_result;

    uint16_t actual_fan_rpm;
    uint16_t applied_fan_duty_x100;
    int16_t  fan_error_rpm;

    uint32_t last_fan_control_tick;
    uint32_t fan_adjust_count;
    uint32_t fan_fault_count;

    uint8_t  damper_control_state;
    uint8_t  damper_state;
    uint8_t  damper_position_valid;
    uint8_t  damper_control_result;
    uint8_t  damper_auto_owned;
    uint8_t  damper_failsafe_stop_issued;
    uint16_t reserved_damper;

    int32_t  actual_damper_steps;
    int32_t  requested_damper_target_steps;
    int32_t  last_commanded_damper_steps;
    int32_t  damper_error_steps;

    uint32_t last_damper_control_tick;
    uint32_t damper_command_count;
    uint32_t damper_busy_count;
    uint32_t damper_fault_count;
} AppAutoControlSnapshot;

void AppAutoControl_Init(void);
void AppAutoControl_Process(void);
bool AppAutoControl_GetSnapshot(AppAutoControlSnapshot *snap);

uint8_t AppAutoControl_SetMode(uint8_t mode);
uint8_t AppAutoControl_GetMode(void);

#endif
