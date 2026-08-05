/**
 * @file app_damper_config.h
 * @brief 风门硬件方向、行程与上电校准配置。
 *
 * 配置职责：根据 Master/Slave 角色决定是否编译风门，定义 1700 步行程、300 PPS、开关方向、上电全开校准和 TIM6 参数。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_DAMPER_CONFIG_H
#define APP_DAMPER_CONFIG_H

#include "app_onewire_config.h"

#if (APP_ONEWIRE_ROLE == APP_ONEWIRE_ROLE_MASTER)
#define APP_DAMPER_ENABLED  1
#else
#define APP_DAMPER_ENABLED  0
#endif

#define APP_DAMPER_FULL_TRAVEL_STEPS       1700U
#define APP_DAMPER_STEP_PPS                 300U
#define APP_DAMPER_DIAG_MAX_RELATIVE_STEPS  100U
#define APP_DAMPER_POST_MOVE_HOLD_MS        100U
#define APP_DAMPER_FORWARD_IS_OPEN            0U  /* 实物确认: 正向=关闭 */

/* 上电无条件沿打开方向运行全行程，完成后把逻辑位置建立为0步（全开）。 */
#define APP_DAMPER_BOOT_HOMING_ENABLED         1U
#define APP_DAMPER_BOOT_HOMING_STEPS          APP_DAMPER_FULL_TRAVEL_STEPS

#if (APP_DAMPER_FORWARD_IS_OPEN > 1U)
#error "APP_DAMPER_FORWARD_IS_OPEN must be 0 or 1"
#endif

#if (APP_DAMPER_BOOT_HOMING_ENABLED > 1U)
#error "APP_DAMPER_BOOT_HOMING_ENABLED must be 0 or 1"
#endif

#if (APP_DAMPER_BOOT_HOMING_STEPS == 0U)
#error "APP_DAMPER_BOOT_HOMING_STEPS must be greater than zero"
#endif

#define APP_DAMPER_TIMER_CLOCK_HZ       84000000UL
#define APP_DAMPER_TIM_PSC                   279U
#define APP_DAMPER_TIM_ARR                   999U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
