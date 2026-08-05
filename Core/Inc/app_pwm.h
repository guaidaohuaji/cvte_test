/**
 * @file app_pwm.h
 * @brief TIM4_CH1 通用 PWM 输出（公共接口头文件）。
 *
 * 模块职责：根据目标频率和整数百分比占空比计算 PSC/ARR/CCR，原子更新 TIM4，并保存目标值、实际值和频率误差。
 * 数据输入：对象 0x01 控制命令传入的 enabled、frequency_hz 和 duty_x100。
 * 数据输出：PB6/TIM4_CH1 波形；对象 0x01 查询使用的配置快照。
 * 执行上下文：仅由主循环调用；更新期间会短暂停止 TIM4，并在启动失败时回滚旧配置。
 * 阅读重点：重点理解 period_counts=ARR+1、PSC+1、1%量化、0/100%强制电平以及失败回滚。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_PWM_H
#define APP_PWM_H

#include <stdbool.h>
#include <stdint.h>

#define APP_PWM_MIN_FREQUENCY_HZ 1U
#define APP_PWM_MAX_FREQUENCY_HZ 840000U
#define APP_PWM_DUTY_STEP_X100    100U

bool AppPwm_Init(void);
bool AppPwm_Configure(bool enabled, uint32_t frequency_hz, uint16_t duty_x100);
bool AppPwm_SetFrequency(uint32_t frequency_hz);
bool AppPwm_SetDutyX100(uint16_t duty_x100);
bool AppPwm_Enable(bool enabled);
bool AppPwm_IsEnabled(void);

uint32_t AppPwm_GetTargetFrequency(void);
uint32_t AppPwm_GetActualFrequency(void);
int32_t  AppPwm_GetFrequencyErrorPpm(void);

uint16_t AppPwm_GetTargetDutyX100(void);
uint16_t AppPwm_GetActualDutyX100(void);

uint32_t AppPwm_GetPrescaler(void);
uint32_t AppPwm_GetAutoReload(void);
uint32_t AppPwm_GetCompare(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
