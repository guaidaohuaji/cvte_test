/**
 * @file app_auto_control_config.h
 * @brief 温度 AUTO 控制门限与节拍配置。
 *
 * 配置职责：定义温度映射范围、风机 RPM 范围、风门冷热端点、控制周期、闭环步长、容差迟滞和故障安全目标。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_AUTO_CONTROL_CONFIG_H
#define APP_AUTO_CONTROL_CONFIG_H

#include "app_onewire_config.h"
#include "app_fan_config.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
#define APP_AUTO_CONTROL_ENABLED  1
#else
#define APP_AUTO_CONTROL_ENABLED  0
#endif

#define APP_AUTO_CONTROL_PERIOD_MS            1000U

#define APP_AUTO_TEMP_MIN_CENTI_C            (-2500)
#define APP_AUTO_TEMP_MAX_CENTI_C             6000

#define APP_AUTO_FAN_MIN_RPM                  1000U
#define APP_AUTO_FAN_MAX_RPM                  2300U

#define APP_AUTO_DAMPER_COLD_STEPS            1700
#define APP_AUTO_DAMPER_HOT_STEPS                0

#define APP_AUTO_TEMP_STARTUP_GRACE_MS        1000U

#define APP_AUTO_DEFAULT_MODE_MANUAL             1U

/* Fan automatic control: feed-forward profile + bounded closed-loop trim. */
#define APP_AUTO_FAN_CONTROL_PERIOD_MS        1000U
#define APP_AUTO_FAN_TOLERANCE_ENTER_RPM        50U
#define APP_AUTO_FAN_TOLERANCE_EXIT_RPM         80U
#define APP_AUTO_FAN_FEEDFORWARD_DELTA_RPM      100U
#define APP_AUTO_FAN_ERROR_CONFIRM_CYCLES         2U

#define APP_AUTO_FAN_FINE_ERROR_MAX_RPM         120U
#define APP_AUTO_FAN_MEDIUM_ERROR_MAX_RPM       250U
#define APP_AUTO_FAN_FINE_STEP_X100              100U
#define APP_AUTO_FAN_MEDIUM_STEP_X100            200U
#define APP_AUTO_FAN_LARGE_STEP_X100             300U

#define APP_AUTO_FAN_DUTY_QUANTUM_X100           100U
#define APP_AUTO_FAN_NORMAL_MIN_DUTY_X100       1000U
#define APP_AUTO_FAN_NORMAL_MAX_DUTY_X100       8000U
#define APP_AUTO_FAN_FAILSAFE_DUTY_X100        10000U

#define APP_AUTO_DAMPER_CONTROL_PERIOD_MS     1000U
#define APP_AUTO_DAMPER_DEADBAND_STEPS          20U
#define APP_AUTO_DAMPER_FAILSAFE_STEPS            0
#define APP_AUTO_DAMPER_STOP_ON_MANUAL           1U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
