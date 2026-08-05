/**
 * @file app_fan_health_config.h
 * @brief 风机健康判定门限与时序配置。
 *
 * 配置职责：定义 600/450 RPM 偏差迟滞、5 秒确认、启动/调速 settling 时间和监测占空比范围。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_FAN_HEALTH_CONFIG_H
#define APP_FAN_HEALTH_CONFIG_H

#include "app_auto_control_config.h"

/*
 * Step19 phase-2 promotes the Step18 shadow decision to a formal safety
 * action. A confirmed speed-deviation or tach-loss fault is latched and the
 * fan is forced to 0%. Restart stays inhibited after fault clear until an
 * explicit operator command authorizes it.
 */
#define APP_FAN_HEALTH_MONITOR_ENABLE             1U
#define APP_FAN_HEALTH_DRIVES_OUTPUT              1U

#define APP_FAN_HEALTH_DEVIATION_ENTER_RPM      600U
#define APP_FAN_HEALTH_DEVIATION_CLEAR_RPM      450U
#define APP_FAN_HEALTH_CONFIRM_MS              5000U
#define APP_FAN_HEALTH_POST_BOOST_SETTLE_MS    5000U
#define APP_FAN_HEALTH_DUTY_CHANGE_SETTLE_MS   3000U
#define APP_FAN_HEALTH_LARGE_DUTY_STEP_X100     500U

#define APP_FAN_HEALTH_MIN_DUTY_X100 \
    APP_AUTO_FAN_NORMAL_MIN_DUTY_X100
#define APP_FAN_HEALTH_MAX_DUTY_X100 \
    APP_AUTO_FAN_NORMAL_MAX_DUTY_X100

#if (APP_FAN_HEALTH_DRIVES_OUTPUT != 1U)
#error "Step19 phase-2 requires formal fan-health shutdown output control"
#endif

#if (APP_FAN_HEALTH_DEVIATION_CLEAR_RPM >= \
     APP_FAN_HEALTH_DEVIATION_ENTER_RPM)
#error "Fan health deviation clear threshold must be below enter threshold"
#endif

#if (APP_FAN_HEALTH_CONFIRM_MS == 0U) || \
    (APP_FAN_HEALTH_POST_BOOST_SETTLE_MS == 0U) || \
    (APP_FAN_HEALTH_DUTY_CHANGE_SETTLE_MS == 0U)
#error "Fan health timing values must be non-zero"
#endif

#if (APP_FAN_HEALTH_MIN_DUTY_X100 >= APP_FAN_HEALTH_MAX_DUTY_X100)
#error "Fan health duty range is invalid"
#endif


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
