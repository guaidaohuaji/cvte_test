/**
 * @file app_fan_config.h
 * @brief 风机底层 PWM 与测速配置。
 *
 * 配置职责：定义 TIM10 PWM 频率、启动加力时间、允许占空比、FG 到 RPM 的倍率和无测速超时。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_FAN_CONFIG_H
#define APP_FAN_CONFIG_H

#define APP_FAN_PWM_FREQUENCY_HZ        10000U
#define APP_FAN_PWM_TIMER_CLOCK_HZ      168000000UL
#define APP_FAN_PWM_PRESCALER           167U
#define APP_FAN_PWM_PERIOD              99U

#define APP_FAN_STARTUP_BOOST_MS        5000U

#define APP_FAN_MIN_DUTY_X100           1000U
#define APP_FAN_MAX_DUTY_X100           10000U

#define APP_FAN_RPM_FACTOR              30U
#define APP_FAN_NO_TACH_TIMEOUT_MS      500U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
