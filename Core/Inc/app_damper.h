/**
 * @file app_damper.h
 * @brief TB6612 两相步进风门控制（公共接口头文件）。
 *
 * 模块职责：使用 TIM6 300 PPS 节拍驱动四拍相序，管理绝对/相对位置、上电全开校准、到位保持、释放和故障。
 * 数据输入：对象 0x07 命令、AUTO 风门目标和 TIM6 周期回调。
 * 数据输出：GPIOB/GPIOC 相线、STBY、位置和状态快照。
 * 执行上下文：真正换相在 TIM6 回调；主循环负责启动、完成、保持计时和命令状态管理。
 * 阅读重点：先看相位表与方向定义，再看 damper_start_motion()，然后阅读 boot homing 和 AppDamper_Process() 状态转换。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

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
    DAMPER_STATE_BOOT_HOMING      = 0x08,
    DAMPER_STATE_UNAVAILABLE      = 0xFE
} DamperState;

typedef enum {
    DAMPER_CMD_NONE          = 0x00,
    DAMPER_CMD_MOVE_ABSOLUTE = 0x01,
    DAMPER_CMD_MOVE_RELATIVE = 0x02,
    DAMPER_CMD_BOOT_HOMING   = 0x03
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


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
