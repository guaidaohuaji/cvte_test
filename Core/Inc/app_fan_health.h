/**
 * @file app_fan_health.h
 * @brief 风机异常判定、故障锁存与停机（公共接口头文件）。
 *
 * 模块职责：根据实际 PWM 查表得到预期 RPM，检测持续转速偏差或测速丢失，经过 settling 和 5 秒确认后锁存并停机。
 * 数据输入：AppFan 快照、标定表预期 RPM、控制模式和时间。
 * 数据输出：健康状态/故障现场快照；对 AppFan 的安全停机、清锁存和重启授权。
 * 执行上下文：每轮主循环执行；故障判定完全在 MCU 侧完成，上位机只查询和清除。
 * 阅读重点：按 settling→suspect→confirm→latch 四阶段阅读，注意 600/450 RPM 迟滞和清故障不等于授权重启。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_FAN_HEALTH_H
#define APP_FAN_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_FAN_HEALTH_STATE_DISABLED = 0,
    APP_FAN_HEALTH_STATE_STARTUP_BOOST,
    APP_FAN_HEALTH_STATE_SETTLING,
    APP_FAN_HEALTH_STATE_NORMAL,
    APP_FAN_HEALTH_STATE_SPEED_SUSPECT,
    APP_FAN_HEALTH_STATE_TACH_SUSPECT,
    APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED,
    APP_FAN_HEALTH_STATE_TACH_FAULT_LATCHED
} AppFanHealthState;

typedef enum {
    APP_FAN_HEALTH_FAULT_NONE = 0,
    APP_FAN_HEALTH_FAULT_SPEED_LOW,
    APP_FAN_HEALTH_FAULT_SPEED_HIGH,
    APP_FAN_HEALTH_FAULT_TACH_LOST
} AppFanHealthFaultType;

typedef struct {
    uint8_t  state;
    uint8_t  fault_type;
    uint8_t  fault_latched;
    uint8_t  monitoring_active;
    uint8_t  tach_valid;
    uint8_t  shutdown_succeeded;
    uint8_t  restart_inhibited;
    uint8_t  reserved0;

    uint16_t applied_duty_x100;
    uint16_t reference_duty_x100;
    uint16_t expected_rpm;
    uint16_t actual_rpm;
    int16_t  deviation_rpm;
    uint16_t absolute_deviation_rpm;
    uint16_t maximum_absolute_deviation_rpm;

    uint16_t fault_applied_duty_x100;
    uint16_t fault_expected_rpm;
    uint16_t fault_actual_rpm;
    int16_t  fault_deviation_rpm;

    uint32_t abnormal_elapsed_ms;
    uint32_t settling_remaining_ms;
    uint32_t update_count;
    uint32_t suspect_count;
    uint32_t fault_count;
    uint32_t shutdown_count;
    uint32_t clear_count;
    uint32_t reset_count;
} AppFanHealthSnapshot;

void AppFanHealth_Init(void);
void AppFanHealth_Process(void);
bool AppFanHealth_GetSnapshot(AppFanHealthSnapshot *snapshot);

/* Clears a latched health fault but deliberately leaves restart inhibited.
 * The fan remains off until an explicit operator command authorizes restart. */
bool AppFanHealth_ClearFault(void);

/* Called only from an explicit manual command or an explicit AUTO-mode
 * command after the fault has been cleared. */
bool AppFanHealth_AuthorizeRestart(void);

bool AppFanHealth_IsFaultLatched(void);
bool AppFanHealth_RestartIsInhibited(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
