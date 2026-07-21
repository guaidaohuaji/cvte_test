#include "app_ntc.h"
#include "app_ntc_config.h"
#include "app_adc_scan.h"
#include "stm32f4xx_hal.h"
#include <math.h>
#include <stddef.h>

static AppNtcSnapshot snap;
static uint32_t last_sample_tick;
static uint32_t last_ok_tick;
static bool init_ok;

static bool check_config(void)
{
    if ((APP_ADC_FULL_SCALE_COUNTS == 0U) ||
        (APP_NTC_R_TOP_OHM == 0U) ||
        (APP_NTC_R_MID_OHM == 0U) ||
        (APP_NTC_R_BOTTOM_OHM == 0U) ||
        (APP_NTC_REFERENCE_R_OHM == 0U) ||
        (APP_NTC_B_VALUE_K == 0U) ||
        (APP_NTC_SAMPLE_COUNT == 0U) ||
        (APP_ADC_REFERENCE_MV == 0U) ||
        (APP_NTC_MIN_TEMP_CENTI_C >= APP_NTC_MAX_TEMP_CENTI_C)) { return false; }
    return true;
}

static bool calc_resistance(uint16_t adc, uint32_t *r)
{
    if (r == NULL || adc == 0U) return false;

    uint64_t top    = (uint64_t)APP_NTC_R_BOTTOM_OHM * (uint64_t)APP_ADC_FULL_SCALE_COUNTS;
    uint64_t total  = (top + (uint64_t)adc / 2ULL) / (uint64_t)adc;
    uint64_t fixed  = (uint64_t)APP_NTC_R_TOP_OHM +
                      (uint64_t)APP_NTC_R_MID_OHM +
                      (uint64_t)APP_NTC_R_BOTTOM_OHM;

    if (total <= fixed) return false;
    total -= fixed;
    if (total > UINT32_MAX) return false;
    *r = (uint32_t)total;
    return true;
}

static uint16_t calc_voltage_mv(uint16_t adc)
{
    uint32_t v = (uint32_t)adc * APP_ADC_REFERENCE_MV;
    v = (v + APP_ADC_FULL_SCALE_COUNTS / 2U) / APP_ADC_FULL_SCALE_COUNTS;
    return (uint16_t)v;
}

static bool calc_temp(int16_t *out, uint32_t r_ohm)
{
    if (out == NULL || r_ohm == 0U) return false;

    float ratio = (float)r_ohm / (float)APP_NTC_REFERENCE_R_OHM;
    float ln    = logf(ratio);
    float tk    = 1.0f / (1.0f / ((float)APP_NTC_REFERENCE_TEMP_MK / 1000.0f)
                          + ln / (float)APP_NTC_B_VALUE_K);
    float tc    = tk - 273.15f;
    int32_t ci  = (int32_t)(tc * 100.0f + ((tc >= 0.0f) ? 0.5f : -0.5f));

    if (ci < INT16_MIN || ci > INT16_MAX) return false;
    *out = (int16_t)ci;
    return true;
}

static bool do_sample(uint16_t *adc_out)
{
    uint32_t ntc_count = 0U;
    if (adc_out == NULL) return false;
    if (!AppAdcScan_GetNtcAverage(adc_out, &ntc_count)) return false;
    return true;
}

bool AppNtc_Init(void)
{
    snap.state = APP_NTC_STATE_SEARCH;
    snap.adc_raw = 0U;
    snap.voltage_mv = 0U;
    snap.resistance_ohm = 0U;
    snap.temp_centi_c = 0;
    snap.age_ms = 65535U;
    last_sample_tick = 0U;
    last_ok_tick = 0U;
    init_ok = false;

    if (!check_config())
    {
        snap.state = APP_NTC_STATE_CONFIG_ERROR;
        return false;
    }

    init_ok = true;
    return true;
}

void AppNtc_Process(void)
{
    if (!init_ok) return;

    uint32_t now = HAL_GetTick();
    if ((now - last_sample_tick) < APP_NTC_SAMPLE_INTERVAL_MS) return;
    last_sample_tick = now;

    uint16_t adc = 0U;
    if (!do_sample(&adc))
    {
        snap.state = APP_NTC_STATE_ADC_ERROR;
        return;
    }

    snap.adc_raw    = adc;
    snap.voltage_mv = calc_voltage_mv(adc);

    uint32_t r = 0U;
    if (!calc_resistance(adc, &r))
    {
        snap.resistance_ohm = 0U;
        snap.temp_centi_c   = 0;
        if (adc == 0U)
        {
            snap.state = APP_NTC_STATE_OPEN_OR_UNDER_TEMP;
        }
        else
        {
            snap.state = APP_NTC_STATE_SHORT_OR_OVER_TEMP;
        }
        return;
    }

    snap.resistance_ohm = r;

    int16_t tc = 0;
    if (!calc_temp(&tc, r))
    {
        snap.temp_centi_c = 0;
        snap.state = APP_NTC_STATE_CALC_ERROR;
        return;
    }

    snap.temp_centi_c = tc;

    if (tc < APP_NTC_MIN_TEMP_CENTI_C)
    {
        snap.state = APP_NTC_STATE_OPEN_OR_UNDER_TEMP;
        return;
    }
    if (tc > APP_NTC_MAX_TEMP_CENTI_C)
    {
        snap.state = APP_NTC_STATE_SHORT_OR_OVER_TEMP;
        return;
    }

    snap.state = APP_NTC_STATE_OK;
    last_ok_tick = now;
}

bool AppNtc_GetSnapshot(AppNtcSnapshot *s)
{
    if (s == NULL) return false;

    uint32_t now = HAL_GetTick();
    uint32_t age;

    if (snap.state == APP_NTC_STATE_OK)
    {
        age = (now >= last_ok_tick) ? (now - last_ok_tick) : 0U;
    }
    else
    {
        age = 65535U;
    }
    snap.age_ms = (age > 65535U) ? 65535U : (uint16_t)age;

    *s = snap;
    return true;
}
