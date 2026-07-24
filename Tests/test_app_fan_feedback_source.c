#include "app_adc_scan.h"
#include "app_fan_feedback_adc.h"
#include "app_fan_feedback_bpf.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_INTERLEAVED_COUNT 1024U
#define TEST_CH0_SAMPLES       (TEST_INTERLEAVED_COUNT / 2U)

static uint16_t adc_buffer[TEST_INTERLEAVED_COUNT];
static uint32_t adc_seq;
static uint64_t generated_sample_index;
static AppFanFeedbackBpfStats fake_bpf_stats;

static void prepare_constant_block(uint16_t value)
{
    for (uint32_t i = 0U; i < TEST_CH0_SAMPLES; i++)
    {
        adc_buffer[2U * i] = value;
        adc_buffer[2U * i + 1U] = 0U;
    }
    adc_seq++;
}

static void prepare_square_block(void)
{
    /* 50 Hz at 10 kHz: 200 samples per period, 50% duty. */
    for (uint32_t i = 0U; i < TEST_CH0_SAMPLES; i++)
    {
        uint32_t phase = (uint32_t)(generated_sample_index % 200ULL);
        adc_buffer[2U * i] = (phase < 100U) ? 3200U : 500U;
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

void AppFanFeedbackBpf_Init(void)
{
}

void AppFanFeedbackBpf_Reset(void)
{
    fake_bpf_stats.tach_valid = 0U;
    fake_bpf_stats.tach_freq_millihz = 0U;
    fake_bpf_stats.tach_period_samples = 0U;
}

void AppFanFeedbackBpf_BeginBlock(void)
{
}

void AppFanFeedbackBpf_ProcessSample(uint16_t sample)
{
    (void)sample;
}

void AppFanFeedbackBpf_EndBlock(void)
{
}

bool AppFanFeedbackBpf_GetStats(AppFanFeedbackBpfStats *stats)
{
    if (stats == NULL)
    {
        return false;
    }
    *stats = fake_bpf_stats;
    return true;
}

static AppFanFeedbackSnapshot get_snapshot(void)
{
    AppFanFeedbackSnapshot snapshot;
    assert(AppFanFeedback_GetSnapshot(&snapshot));
    return snapshot;
}

int main(void)
{
    AppFanFeedbackSnapshot snapshot;
    uint32_t first_official_seq;

    memset(&fake_bpf_stats, 0, sizeof(fake_bpf_stats));
    adc_seq = 0U;
    generated_sample_index = 0ULL;

    AppFanFeedback_ResetMeasurement();
    snapshot = get_snapshot();
    assert(snapshot.state == 0U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE);

    prepare_constant_block(2048U);
    AppFanFeedback_Process();
    snapshot = get_snapshot();
    assert(snapshot.state == 0U);

    fake_bpf_stats.tach_valid = 1U;
    fake_bpf_stats.tach_freq_millihz = 33333U;
    fake_bpf_stats.tach_period_samples = 300U;
    fake_bpf_stats.tach_update_seq = 1U;

    prepare_constant_block(2048U);
    AppFanFeedback_Process();
    snapshot = get_snapshot();

#if APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF);
    assert(snapshot.freq_millihz == 33333U);
    assert(snapshot.period_samples == 300U);
    first_official_seq = snapshot.update_seq;
    assert(first_official_seq != 0U);

    /* Re-reading the same BPF update must not create a fake fresh sample. */
    prepare_constant_block(2048U);
    AppFanFeedback_Process();
    snapshot = get_snapshot();
    assert(snapshot.update_seq == first_official_seq);

    fake_bpf_stats.tach_freq_millihz = 50000U;
    fake_bpf_stats.tach_period_samples = 200U;
    fake_bpf_stats.tach_update_seq = 2U;
    prepare_constant_block(2048U);
    AppFanFeedback_Process();
    snapshot = get_snapshot();
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF);
    assert(snapshot.freq_millihz == 50000U);
    assert(snapshot.update_seq != first_official_seq);
#else
    (void)first_official_seq;
    assert(snapshot.state == 0U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE);
#endif

    /* Make the BPF invalid, then let the legacy detector acquire 50 Hz. */
    fake_bpf_stats.tach_valid = 0U;
    fake_bpf_stats.tach_freq_millihz = 0U;
    fake_bpf_stats.tach_period_samples = 0U;

    for (uint32_t block = 0U; block < 12U; block++)
    {
        prepare_square_block();
        AppFanFeedback_Process();
    }
    snapshot = get_snapshot();

#if (APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY) && \
    APP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY);
    assert(snapshot.legacy_fallback_active == 1U);
    assert(snapshot.freq_millihz > 49000U);
    assert(snapshot.freq_millihz < 51000U);
#elif APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_LEGACY
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY);
    assert(snapshot.legacy_fallback_active == 0U);
    assert(snapshot.freq_millihz > 49000U);
    assert(snapshot.freq_millihz < 51000U);
#else
    assert(snapshot.state == 0U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE);
#endif

    /* A fresh valid BPF result regains priority only in BPF-primary mode. */
    fake_bpf_stats.tach_valid = 1U;
    fake_bpf_stats.tach_freq_millihz = 60000U;
    fake_bpf_stats.tach_period_samples = 167U;
    fake_bpf_stats.tach_update_seq = 3U;
    prepare_square_block();
    AppFanFeedback_Process();
    snapshot = get_snapshot();

#if APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF);
    assert(snapshot.freq_millihz == 60000U);
    assert(snapshot.legacy_fallback_active == 0U);
#else
    assert(snapshot.state == 1U);
    assert(snapshot.active_source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY);
#endif

    puts("fan feedback source arbitration tests passed");
    return 0;
}
