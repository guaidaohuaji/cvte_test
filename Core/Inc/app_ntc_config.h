#ifndef APP_NTC_CONFIG_H
#define APP_NTC_CONFIG_H

#define APP_NTC_RATIOMETRIC              1U

#define APP_NTC_SUPPLY_MV                3300U
#define APP_ADC_REFERENCE_MV             3300U
#define APP_ADC_FULL_SCALE_COUNTS        4095U

#define APP_NTC_R_TOP_OHM                5240U
#define APP_NTC_R_MID_OHM                10000U
#define APP_NTC_R_BOTTOM_OHM             10000U

#define APP_NTC_REFERENCE_R_OHM          5060U
#define APP_NTC_REFERENCE_TEMP_MK        278150U
#define APP_NTC_B_VALUE_K                3839U

#define APP_NTC_MIN_TEMP_CENTI_C         0
#define APP_NTC_MAX_TEMP_CENTI_C         6000

#define APP_NTC_SAMPLE_INTERVAL_MS       250U
#define APP_NTC_SAMPLE_COUNT             32U
#define APP_NTC_ADC_TIMEOUT_MS           2U

#endif
