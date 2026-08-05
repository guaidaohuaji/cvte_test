/**
 * @file app_auto_control.h
 * @brief 温度映射的风机与风门自动控制（公共接口头文件）。
 *
 * 模块职责：把有效温度映射为目标风机转速和风门位置，并分别驱动风机闭环和风门位置状态机。
 * 数据输入：NTC 快照、风机快照、风门快照、AUTO/MANUAL 模式命令。
 * 数据输出：目标 RPM、目标风门步数、执行命令和完整控制诊断快照。
 * 执行上下文：每秒评估目标；风机与风门控制各有独立节拍和状态，均为非阻塞。
 * 阅读重点：先看 evaluate_targets() 的线性映射，再分别看 fan_control_process() 与 damper_control_process()，最后看模式切换。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

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
    APP_AUTO_FAN_CTRL_SAFETY_LOCKED  = 9,
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


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
