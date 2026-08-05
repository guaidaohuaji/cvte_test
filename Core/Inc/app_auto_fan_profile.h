/**
 * @file app_auto_fan_profile.h
 * @brief 风机占空比/RPM 标定表与插值工具（公共接口头文件）。
 *
 * 模块职责：保存实测标定点，在 RPM→占空比和占空比→预期 RPM 两个方向做线性插值，并提供步长和迟滞工具。
 * 数据输入：目标 RPM、当前占空比、绝对转速误差。
 * 数据输出：量化占空比、预期 RPM、调节步长和容差判定。
 * 执行上下文：全部为 static inline，无运行时全局状态，被 AUTO、手动转速和健康诊断共同复用。
 * 阅读重点：重点理解两个相邻标定点之间的整数线性插值与 1%量化。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_AUTO_FAN_PROFILE_H
#define APP_AUTO_FAN_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_auto_control_config.h"

typedef struct {
    uint16_t rpm;
    uint16_t duty_x100;
} AppAutoFanCalibrationPoint;

static const AppAutoFanCalibrationPoint app_auto_fan_profile[] = {
    {1000U, 1000U},
    {1200U, 1500U},
    {1400U, 2000U},
    {1700U, 3000U},
    {1900U, 4000U},
    {2100U, 5000U},
    {2200U, 6000U},
    {2300U, 8000U}
};

#define APP_AUTO_FAN_PROFILE_COUNT \
    ((uint32_t)(sizeof(app_auto_fan_profile) / \
                sizeof(app_auto_fan_profile[0])))

static inline uint16_t AppAutoFan_QuantizeDutyX100(uint32_t duty_x100)
{
    const uint32_t quantum = APP_AUTO_FAN_DUTY_QUANTUM_X100;
    uint32_t quantized;

    if (quantum == 0U)
    {
        return (duty_x100 > 65535U) ? 65535U : (uint16_t)duty_x100;
    }

    quantized = ((duty_x100 + quantum / 2U) / quantum) * quantum;
    if (quantized < APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        quantized = APP_AUTO_FAN_NORMAL_MIN_DUTY_X100;
    if (quantized > APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        quantized = APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;

    return (uint16_t)quantized;
}

static inline uint16_t AppAutoFan_EstimateDutyX100(uint16_t target_rpm)
{
    const AppAutoFanCalibrationPoint *profile = app_auto_fan_profile;
    const uint32_t count = APP_AUTO_FAN_PROFILE_COUNT;
    uint32_t i;

    if (target_rpm <= profile[0].rpm)
        return AppAutoFan_QuantizeDutyX100(profile[0].duty_x100);
    if (target_rpm >= profile[count - 1U].rpm)
        return AppAutoFan_QuantizeDutyX100(profile[count - 1U].duty_x100);

    for (i = 0U; i + 1U < count; ++i)
    {
        const AppAutoFanCalibrationPoint *lo = &profile[i];
        const AppAutoFanCalibrationPoint *hi = &profile[i + 1U];

        if (target_rpm <= hi->rpm)
        {
            uint32_t rpm_span = (uint32_t)hi->rpm - (uint32_t)lo->rpm;
            uint32_t rpm_offset = (uint32_t)target_rpm - (uint32_t)lo->rpm;
            uint32_t duty_span = (uint32_t)hi->duty_x100 - (uint32_t)lo->duty_x100;
            uint32_t interpolated = (uint32_t)lo->duty_x100
                                  + (rpm_offset * duty_span + rpm_span / 2U) / rpm_span;
            return AppAutoFan_QuantizeDutyX100(interpolated);
        }
    }

    return APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;
}

static inline uint16_t AppAutoFan_EstimateRpmFromDutyX100(uint16_t duty_x100)
{
    const AppAutoFanCalibrationPoint *profile = app_auto_fan_profile;
    const uint32_t count = APP_AUTO_FAN_PROFILE_COUNT;
    uint32_t i;

    if (duty_x100 <= profile[0].duty_x100)
        return profile[0].rpm;
    if (duty_x100 >= profile[count - 1U].duty_x100)
        return profile[count - 1U].rpm;

    for (i = 0U; i + 1U < count; ++i)
    {
        const AppAutoFanCalibrationPoint *lo = &profile[i];
        const AppAutoFanCalibrationPoint *hi = &profile[i + 1U];

        if (duty_x100 <= hi->duty_x100)
        {
            uint32_t duty_span = (uint32_t)hi->duty_x100
                               - (uint32_t)lo->duty_x100;
            uint32_t duty_offset = (uint32_t)duty_x100
                                 - (uint32_t)lo->duty_x100;
            uint32_t rpm_span = (uint32_t)hi->rpm - (uint32_t)lo->rpm;
            uint32_t interpolated = (uint32_t)lo->rpm
                                  + (duty_offset * rpm_span + duty_span / 2U)
                                  / duty_span;
            return (interpolated > UINT16_MAX)
                 ? UINT16_MAX
                 : (uint16_t)interpolated;
        }
    }

    return APP_AUTO_FAN_MAX_RPM;
}

static inline uint16_t AppAutoFan_SelectTrimStepX100(uint16_t absolute_error_rpm)
{
    if (absolute_error_rpm <= APP_AUTO_FAN_FINE_ERROR_MAX_RPM)
        return APP_AUTO_FAN_FINE_STEP_X100;
    if (absolute_error_rpm <= APP_AUTO_FAN_MEDIUM_ERROR_MAX_RPM)
        return APP_AUTO_FAN_MEDIUM_STEP_X100;
    return APP_AUTO_FAN_LARGE_STEP_X100;
}

static inline bool AppAutoFan_ErrorIsInTolerance(uint16_t absolute_error_rpm,
                                                 bool already_in_tolerance)
{
    uint16_t limit = already_in_tolerance
                   ? APP_AUTO_FAN_TOLERANCE_EXIT_RPM
                   : APP_AUTO_FAN_TOLERANCE_ENTER_RPM;
    return absolute_error_rpm <= limit;
}


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
