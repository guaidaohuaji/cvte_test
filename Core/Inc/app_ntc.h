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

#endif
