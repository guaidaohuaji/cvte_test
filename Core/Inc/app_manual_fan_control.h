/**
 * @file app_manual_fan_control.h
 * @brief 手动风机 OFF/PWM/目标转速控制（公共接口头文件）。
 *
 * 模块职责：实现手动关闭、直接占空比和目标 RPM 三种模式；目标 RPM 使用标定表前馈与 1/2/3% 闭环微调。
 * 数据输入：对象 0x06 控制命令、AppFan 实际状态和测速。
 * 数据输出：对 AppFan 的启停/占空比命令；手动控制诊断快照。
 * 执行上下文：AUTO 模式拥有更高控制权；检测到 AUTO 后手动状态机会退出并清理历史调节状态。
 * 阅读重点：按 SetTargetRpm()→Process()→误差确认→apply_normal_duty() 的顺序阅读。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

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
    APP_MANUAL_FAN_CTRL_HW_ERROR       = 8,
    APP_MANUAL_FAN_CTRL_SAFETY_LOCKED  = 9
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


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
