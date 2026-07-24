#include "app_adc_scan.h"
#include "app_fan_feedback_adc.h"
#include "app_fan_feedback_bpf.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t fake_bpf_reset_count;

bool AppAdcScan_GetCh0Block(const uint16_t **data,
                            uint32_t *count,
                            uint32_t *seq)
{
    (void)data;
    (void)count;
    (void)seq;
    return false;
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
    fake_bpf_reset_count++;
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
    (void)stats;
    return false;
}

int main(void)
{
    AppFanFeedbackSnapshot snapshot;

    fake_bpf_reset_count = 0U;

    AppFanFeedback_ResetMeasurement();
    assert(fake_bpf_reset_count == 1U);
    assert(AppFanFeedback_GetSnapshot(&snapshot));
    assert(snapshot.state == 0U);
    assert(snapshot.freq_millihz == 0U);
    assert(snapshot.period_samples == 0U);

    AppFanFeedback_ReacquireAfterDutyChange();
    assert(fake_bpf_reset_count == 1U);
    assert(AppFanFeedback_GetSnapshot(&snapshot));
    assert(snapshot.state == 0U);
    assert(snapshot.freq_millihz == 0U);
    assert(snapshot.period_samples == 0U);

    puts("fan feedback reset semantics tests passed");
    return 0;
}
