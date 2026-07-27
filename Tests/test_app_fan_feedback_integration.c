#include "app_adc_scan.h"
#include "app_fan_feedback_adc.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_INTERLEAVED_COUNT 1024U
#define TEST_CH0_SAMPLES       (TEST_INTERLEAVED_COUNT / 2U)
#define TEST_PI                3.14159265358979323846

static uint16_t adc_buffer[TEST_INTERLEAVED_COUNT];
static uint32_t adc_seq;
static uint64_t generated_sample_index;

static uint16_t clamp_adc(double value)
{
    if (value < 0.0) return 0U;
    if (value > 4095.0) return 4095U;
    return (uint16_t)(value + 0.5);
}

static void prepare_signal_block(double frequency_hz, double amplitude)
{
    for (uint32_t i = 0U; i < TEST_CH0_SAMPLES; i++)
    {
        double t = (double)generated_sample_index / 10000.0;
        double sample = 2048.0;

        if (frequency_hz > 0.0)
        {
            sample += amplitude * sin(2.0 * TEST_PI * frequency_hz * t);
            sample += 70.0 * sin(2.0 * TEST_PI * 1000.0 * t);
        }

        adc_buffer[2U * i] = clamp_adc(sample);
        adc_buffer[2U * i + 1U] = 0U;
        generated_sample_index++;
    }
    adc_seq++;
}

bool AppAdcScan_GetCh0Block(const uint16_t **data,
                            uint32_t *count,
                            uint32_t *seq)
{
    if ((data == NULL) || (count == NULL) || (seq == NULL))
    {
        return false;
    }

    *data = adc_buffer;
    *count = TEST_INTERLEAVED_COUNT;
    *seq = adc_seq;
    return true;
}

uint32_t AppAdcScan_GetOverrunCount(void)
{
    return 0U;
}

int main(void)
{
    AppFanFeedbackSnapshot snapshot;

    adc_seq = 0U;
    generated_sample_index = 0ULL;
    AppFanFeedback_ResetMeasurement();

    for (uint32_t block = 0U; block < 30U; block++)
    {
        prepare_signal_block(50.0, 700.0);
        AppFanFeedback_Process();
    }

    assert(AppFanFeedback_GetSnapshot(&snapshot));
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF);
    assert(snapshot.freq_millihz > 49800U);
    assert(snapshot.freq_millihz < 50200U);

    /* The filtered tach should keep working after a large amplitude drop. */
    for (uint32_t block = 0U; block < 12U; block++)
    {
        prepare_signal_block(50.0, 90.0);
        AppFanFeedback_Process();
    }

    assert(AppFanFeedback_GetSnapshot(&snapshot));
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF);
    assert(snapshot.freq_millihz > 49500U);
    assert(snapshot.freq_millihz < 50500U);

    /* After the signal disappears, both detectors must eventually invalidate. */
    for (uint32_t block = 0U; block < 16U; block++)
    {
        prepare_signal_block(0.0, 0.0);
        AppFanFeedback_Process();
    }

    assert(AppFanFeedback_GetSnapshot(&snapshot));
    assert(snapshot.state == 0U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE);

    puts("fan feedback full-chain integration tests passed");
    return 0;
}
