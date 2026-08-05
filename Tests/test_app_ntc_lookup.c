#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_auto_control_config.h"

static uint32_t fake_tick;
static uint16_t fake_adc;
static bool fake_adc_valid;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

bool AppAdcScan_GetNtcAverage(uint16_t *avg, uint32_t *samples)
{
    if (!fake_adc_valid || avg == NULL || samples == NULL)
        return false;
    *avg = fake_adc;
    *samples = 32U;
    return true;
}

/* Include the implementation so this focused unit test can verify the private
 * Rcent table and interpolation helper without expanding the production API. */
#include "../Core/Src/app_ntc.c"

static void expect_lookup(uint32_t resistance_ohm,
                          int16_t expected_temp_centi_c,
                          AppNtcRangeStatus expected_status)
{
    int16_t temp = 0;
    AppNtcRangeStatus status = APP_NTC_RANGE_SENSOR_FAULT;

    assert(calc_temp(&temp, &status, resistance_ohm));
    assert(temp == expected_temp_centi_c);
    assert(status == expected_status);
}

int main(void)
{
    uint32_t i;

    assert(APP_NTC_MIN_TEMP_CENTI_C == -4000);
    assert(APP_NTC_MAX_TEMP_CENTI_C == 12000);
    assert(APP_AUTO_TEMP_MIN_CENTI_C == -2500);
    assert(APP_AUTO_TEMP_MAX_CENTI_C == 6000);
    assert(check_config());

    assert(ntc_rcent_ohm[0] == 64069U);
    assert(ntc_rcent_ohm[15] == 25522U);  /* -25°C */
    assert(ntc_rcent_ohm[40] == 6499U);   /* 0°C */
    assert(ntc_rcent_ohm[45] == 5060U);   /* 5°C */
    assert(ntc_rcent_ohm[65] == 1997U);   /* 25°C */
    assert(ntc_rcent_ohm[100] == 503U);   /* 60°C */
    assert(ntc_rcent_ohm[160] == 84U);    /* 120°C */

    for (i = 1U; i < APP_NTC_TABLE_POINT_COUNT; i++)
        assert(ntc_rcent_ohm[i - 1U] > ntc_rcent_ohm[i]);

    /* Every integer table point must round-trip exactly. */
    for (i = 0U; i < APP_NTC_TABLE_POINT_COUNT; i++)
    {
        int16_t expected = (int16_t)(APP_NTC_TABLE_MIN_TEMP_CENTI_C +
                           (int32_t)i * APP_NTC_TABLE_STEP_CENTI_C);
        expect_lookup(ntc_rcent_ohm[i], expected, APP_NTC_RANGE_IN_RANGE);
    }

    /* Midpoint between 4°C (5317 ohm) and 5°C (5060 ohm) is about 4.50°C. */
    expect_lookup(5189U, 450, APP_NTC_RANGE_IN_RANGE);

    /* Table limits are valid; values beyond them are clamped and classified. */
    expect_lookup(64070U, -4000, APP_NTC_RANGE_CLAMPED_LOW);
    expect_lookup(83U, 12000, APP_NTC_RANGE_CLAMPED_HIGH);

    assert(!calc_temp(NULL, NULL, 1000U));
    assert(!calc_temp(NULL, &(AppNtcRangeStatus){0}, 1000U));
    assert(!calc_temp(&(int16_t){0}, NULL, 1000U));
    assert(!calc_temp(&(int16_t){0}, &(AppNtcRangeStatus){0}, 0U));

    /* Verify the complete ADC -> resistance -> lookup -> state pipeline. */
    fake_adc_valid = true;
    fake_tick = 0U;
    assert(AppNtc_Init());

    fake_adc = 459U;  /* About -40°C with the existing divider. */
    fake_tick = 250U;
    AppNtc_Process();
    {
        AppNtcSnapshot snapshot;
        assert(AppNtc_GetSnapshot(&snapshot));
        assert(snapshot.state == APP_NTC_STATE_OK);
        assert(snapshot.range_status == APP_NTC_RANGE_IN_RANGE);
        assert(snapshot.sensor_measurement_valid);
        assert(snapshot.temp_centi_c >= -4000);
        assert(snapshot.temp_centi_c <= -3900);
    }

    fake_adc = 1617U; /* About 119.5°C; still inside the expanded table. */
    fake_tick = 500U;
    AppNtc_Process();
    {
        AppNtcSnapshot snapshot;
        assert(AppNtc_GetSnapshot(&snapshot));
        assert(snapshot.state == APP_NTC_STATE_OK);
        assert(snapshot.range_status == APP_NTC_RANGE_IN_RANGE);
        assert(snapshot.temp_centi_c >= 11900);
        assert(snapshot.temp_centi_c <= 12000);
    }

    fake_adc = 1618U; /* Calculated resistance is below the 120°C endpoint. */
    fake_tick = 750U;
    AppNtc_Process();
    {
        AppNtcSnapshot snapshot;
        assert(AppNtc_GetSnapshot(&snapshot));
        assert(snapshot.state == APP_NTC_STATE_SHORT_OR_OVER_TEMP);
        assert(snapshot.range_status == APP_NTC_RANGE_CLAMPED_HIGH);
        assert(snapshot.sensor_measurement_valid);
        assert(snapshot.temp_centi_c == 12000);
    }

    fake_adc = 458U;  /* Calculated resistance is above the -40°C endpoint. */
    fake_tick = 1000U;
    AppNtc_Process();
    {
        AppNtcSnapshot snapshot;
        assert(AppNtc_GetSnapshot(&snapshot));
        assert(snapshot.state == APP_NTC_STATE_OPEN_OR_UNDER_TEMP);
        assert(snapshot.range_status == APP_NTC_RANGE_CLAMPED_LOW);
        assert(snapshot.sensor_measurement_valid);
        assert(snapshot.temp_centi_c == -4000);
    }

    puts("NTC Rcent lookup, interpolation, and -40~120C range tests passed");
    return 0;
}
