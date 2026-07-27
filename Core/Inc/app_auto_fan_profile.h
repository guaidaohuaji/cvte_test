#ifndef APP_AUTO_FAN_PROFILE_H
#define APP_AUTO_FAN_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_auto_control_config.h"

typedef struct {
    uint16_t rpm;
    uint16_t duty_x100;
} AppAutoFanCalibrationPoint;

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
    static const AppAutoFanCalibrationPoint profile[] = {
        {1000U, 1000U},
        {1200U, 1500U},
        {1400U, 2000U},
        {1700U, 3000U},
        {1900U, 4000U},
        {2100U, 5000U},
        {2200U, 6000U},
        {2300U, 8000U}
    };
    const uint32_t count = (uint32_t)(sizeof(profile) / sizeof(profile[0]));
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

#endif
