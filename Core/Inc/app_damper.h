#ifndef APP_DAMPER_H
#define APP_DAMPER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_damper_config.h"

typedef enum {
    DAMPER_STATE_UNINITIALIZED    = 0x00,
    DAMPER_STATE_POSITION_UNKNOWN = 0x01,
    DAMPER_STATE_MOVING_FORWARD   = 0x02,
    DAMPER_STATE_MOVING_REVERSE   = 0x03,
    DAMPER_STATE_IDLE_RELEASED    = 0x04,
    DAMPER_STATE_POST_MOVE_HOLD   = 0x05,
    DAMPER_STATE_STOPPED          = 0x06,
    DAMPER_STATE_FAULT            = 0x07,
    DAMPER_STATE_UNAVAILABLE      = 0xFE
} DamperState;

typedef enum {
    DAMPER_CMD_NONE          = 0x00,
    DAMPER_CMD_MOVE_ABSOLUTE = 0x01,
    DAMPER_CMD_MOVE_RELATIVE = 0x02
} DamperCommand;

typedef enum {
    DAMPER_RESULT_SUCCESS              = 0x00,
    DAMPER_RESULT_ABORTED_BY_STOP      = 0x01,
    DAMPER_RESULT_ABORTED_BY_RELEASE   = 0x02,
    DAMPER_RESULT_ABORTED_BY_EMERGENCY = 0x03,
    DAMPER_RESULT_HARDWARE_ERROR       = 0x04,
    DAMPER_RESULT_IN_PROGRESS          = 0x05
} DamperResult;

typedef enum {
    DAMPER_STATUS_OK            = 0x00,
    DAMPER_STATUS_BUSY          = 0x09,
    DAMPER_STATUS_PARAM_RANGE   = 0x05,
    DAMPER_STATUS_NO_VALID_DATA = 0x08,
    DAMPER_STATUS_READ_ONLY     = 0x07,
    DAMPER_STATUS_HW_ERROR      = 0x0A
} DamperStatus;

typedef struct {
    uint8_t  status;
    uint8_t  object_id;
    uint8_t  damper_state;
    uint8_t  flags;
    int32_t  current_steps;
    int32_t  target_steps;
    uint32_t remaining_steps;
    uint16_t full_travel_steps;
    uint16_t configured_pps;
    uint8_t  last_command;
    uint8_t  last_result;
    uint8_t  fault_flags;
} DamperSnapshot;

void AppDamper_Init(void);
void AppDamper_Process(void);
void AppDamper_TimerCallback(void);
void AppDamper_EmergencyShutdown(void);
bool AppDamper_GetSnapshot(DamperSnapshot *snap);

uint8_t AppDamper_MoveAbsolute(int32_t target);
uint8_t AppDamper_MoveRelative(int32_t delta);
uint8_t AppDamper_SetCurrentPosition(int32_t position);
uint8_t AppDamper_Stop(void);
uint8_t AppDamper_Release(void);

#endif
