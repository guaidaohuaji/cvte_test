#include "app_auto_fan_profile.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    assert(AppAutoFan_EstimateDutyX100(900U) == 1000U);
    assert(AppAutoFan_EstimateDutyX100(1000U) == 1000U);
    assert(AppAutoFan_EstimateDutyX100(1100U) == 1300U);
    assert(AppAutoFan_EstimateDutyX100(1200U) == 1500U);
    assert(AppAutoFan_EstimateDutyX100(1300U) == 1800U);
    assert(AppAutoFan_EstimateDutyX100(1400U) == 2000U);
    assert(AppAutoFan_EstimateDutyX100(1500U) == 2300U);
    assert(AppAutoFan_EstimateDutyX100(1600U) == 2700U);
    assert(AppAutoFan_EstimateDutyX100(1800U) == 3500U);
    assert(AppAutoFan_EstimateDutyX100(2000U) == 4500U);
    assert(AppAutoFan_EstimateDutyX100(2150U) == 5500U);
    assert(AppAutoFan_EstimateDutyX100(2250U) == 7000U);
    assert(AppAutoFan_EstimateDutyX100(2300U) == 8000U);
    assert(AppAutoFan_EstimateDutyX100(2500U) == 8000U);

    assert(AppAutoFan_EstimateRpmFromDutyX100(500U) == 1000U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(1000U) == 1000U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(1250U) == 1100U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(1500U) == 1200U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(2300U) == 1490U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(3500U) == 1800U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(4500U) == 2000U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(5500U) == 2150U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(7000U) == 2250U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(8000U) == 2300U);
    assert(AppAutoFan_EstimateRpmFromDutyX100(9000U) == 2300U);

    assert(AppAutoFan_SelectTrimStepX100(50U) == 100U);
    assert(AppAutoFan_SelectTrimStepX100(120U) == 100U);
    assert(AppAutoFan_SelectTrimStepX100(121U) == 200U);
    assert(AppAutoFan_SelectTrimStepX100(250U) == 200U);
    assert(AppAutoFan_SelectTrimStepX100(251U) == 300U);

    assert(AppAutoFan_ErrorIsInTolerance(50U, false));
    assert(!AppAutoFan_ErrorIsInTolerance(51U, false));
    assert(AppAutoFan_ErrorIsInTolerance(80U, true));
    assert(!AppAutoFan_ErrorIsInTolerance(81U, true));

    puts("auto fan profile tests passed");
    return 0;
}
