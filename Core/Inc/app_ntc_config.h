/**
 * @file app_ntc_config.h
 * @brief NTC 电气模型与采样配置。
 *
 * 配置职责：定义 ADC 参考、分压电阻、Rcent 查表有效范围和采样周期；修改硬件电阻或热敏电阻型号后必须同步此文件。
 * 阅读方法：先确认单位和硬件时钟，再检查所有 #if/#error 编译期约束。
 * 修改原则：配置常量只保留一份；修改后同步协议说明、测试和实物验证。
 */

#ifndef APP_NTC_CONFIG_H
#define APP_NTC_CONFIG_H

#define APP_NTC_RATIOMETRIC              1U

#define APP_NTC_SUPPLY_MV                3300U
#define APP_ADC_REFERENCE_MV             3300U
#define APP_ADC_FULL_SCALE_COUNTS        4095U

#define APP_NTC_R_TOP_OHM                5240U
#define APP_NTC_R_MID_OHM                10000U
#define APP_NTC_R_BOTTOM_OHM             10000U

/* LTR LNTD5.06(05)GW 数据表 Rcent 列覆盖 -40°C 到 120°C。 */
#define APP_NTC_MIN_TEMP_CENTI_C        (-4000)
#define APP_NTC_MAX_TEMP_CENTI_C         12000

#define APP_NTC_SAMPLE_INTERVAL_MS       250U
#define APP_NTC_SAMPLE_COUNT             32U
#define APP_NTC_ADC_TIMEOUT_MS           2U


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
