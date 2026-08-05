/**
 * @file app_fan.h
 * @brief 风机 PWM 执行层与启动状态机（公共接口头文件）。
 *
 * 模块职责：管理 TIM10 风机 PWM、5 秒启动加力、反馈有效性、无测速状态以及健康模块使用的安全锁。
 * 数据输入：手动/AUTO 控制模块给出的启停和占空比；风机反馈模块给出的频率。
 * 数据输出：PB8/TIM10_CH1 PWM；风机状态、占空比、FG 和 RPM 快照。
 * 执行上下文：所有控制写入最终都汇聚到本模块；健康故障可立即强制 0%，清故障后仍需显式授权重启。
 * 阅读重点：先读 AppFan_SetEnabled()，再读 AppFan_Process() 中 boost、反馈和状态更新，最后看安全锁 API。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

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
    APP_FAN_STATE_CONFIG_ERROR = 6,
    APP_FAN_STATE_SAFETY_LOCKED = 7
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
    uint8_t     tach_valid;
} AppFanSnapshot;

bool AppFan_Init(void);
void AppFan_Process(void);
bool AppFan_SetEnabled(bool enabled, uint16_t duty_x100);
bool AppFan_GetSnapshot(AppFanSnapshot *snap);

/* Safety lock used by the fan-health monitor. Trip immediately forces 0%.
 * Clear removes the fault latch but keeps restart inhibited. Only an explicit
 * operator command should call AppFan_AuthorizeRestart(). */
bool AppFan_TripSafetyFault(void);
bool AppFan_ClearSafetyFault(void);
bool AppFan_AuthorizeRestart(void);
bool AppFan_IsSafetyFaultLatched(void);
bool AppFan_IsRestartInhibited(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
