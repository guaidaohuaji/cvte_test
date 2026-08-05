/**
 * @file app_ntc.h
 * @brief NTC 温度换算与状态分类（公共接口头文件）。
 *
 * 模块职责：周期读取共享 ADC 平均值，计算电压、热敏电阻阻值并按厂家 Rcent 表换算温度，并提供控制温度与故障状态。
 * 数据输入：AppAdcScan_GetNtcAverage() 提供的 ADC 平均值和样本数。
 * 数据输出：ADC、电压、阻值、温度、范围状态和有效性快照。
 * 执行上下文：主循环非阻塞周期任务；配置和数学异常通过状态返回，不阻塞其他功能。
 * 阅读重点：先看 app_ntc_config.h 的电阻网络，再顺着 do_sample()、calc_resistance() 和 calc_temp() 阅读查表、插值与范围判定。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_NTC_H
#define APP_NTC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_NTC_STATE_SEARCH = 0,
    APP_NTC_STATE_OK = 1,
    APP_NTC_STATE_ADC_ERROR = 2,
    APP_NTC_STATE_OPEN_OR_UNDER_TEMP = 3,
    APP_NTC_STATE_SHORT_OR_OVER_TEMP = 4,
    APP_NTC_STATE_CALC_ERROR = 5,
    APP_NTC_STATE_CONFIG_ERROR = 6
} AppNtcState;

typedef enum {
    APP_NTC_RANGE_IN_RANGE = 0,
    APP_NTC_RANGE_CLAMPED_LOW = 1,
    APP_NTC_RANGE_CLAMPED_HIGH = 2,
    APP_NTC_RANGE_SENSOR_FAULT = 3
} AppNtcRangeStatus;

typedef struct {
    AppNtcState state;
    uint16_t adc_raw;
    uint16_t voltage_mv;
    uint32_t resistance_ohm;
    int16_t  temp_centi_c;
    uint16_t age_ms;

    bool sensor_measurement_valid;
    AppNtcRangeStatus range_status;
    int16_t  control_temp_centi_c;
} AppNtcSnapshot;

bool AppNtc_Init(void);
void AppNtc_Process(void);
bool AppNtc_GetSnapshot(AppNtcSnapshot *snap);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
