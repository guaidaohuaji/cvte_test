#include "app_fan_feedback_adc.h"
#include "app_adc_scan.h"
#include <stddef.h>

#define SAMPLE_RATE_HZ          10000U
#define MA_WINDOW               10U
#define LOW_THRESHOLD           900U
#define HIGH_THRESHOLD          1800U
#define MIN_HIGH_SAMPLES        20U
#define MIN_LOW_SAMPLES         5U

#define FG_MIN_FREQ_HZ          10U
#define FG_MAX_FREQ_HZ          120U
#define PERIOD_MIN_SAMPLES      ((SAMPLE_RATE_HZ) / (FG_MAX_FREQ_HZ))
#define PERIOD_MAX_SAMPLES      ((SAMPLE_RATE_HZ) / (FG_MIN_FREQ_HZ))
#define REQUIRED_GOOD_COUNT     3U

typedef enum { CONF_NONE, CONF_LOW, CONF_HIGH } ConfirmedLevel;
typedef enum { CAND_NONE, CAND_HIGH, CAND_LOW } CandidateType;

static AppFanFeedbackSnapshot snap;

static uint32_t last_ch0_seq;
static uint32_t total_idx;

static ConfirmedLevel confirmed_level;
static CandidateType candidate_type;
static uint32_t cand_start_idx;
static uint32_t cand_count;

static uint64_t prev_rise_idx;
static bool     has_prev_rise;
static uint32_t good_count;

static uint32_t raw_min, raw_max;
static uint32_t high_cand_rejected;
static uint32_t low_cand_rejected;
static uint32_t short_period_rejected;
static uint32_t long_period_rejected;
static uint32_t resync_count;
static uint32_t accepted_period_count;
static uint32_t last_accepted_period_samples;

static uint16_t ma_buf[MA_WINDOW];
static uint32_t ma_sum;
static uint8_t  ma_idx;
static uint8_t  ma_cnt;

void AppFanFeedback_ResetMeasurement(void)
{
    confirmed_level = CONF_NONE;
    candidate_type  = CAND_NONE;
    cand_start_idx  = 0U;
    cand_count      = 0U;

    has_prev_rise = false;
    prev_rise_idx = 0ULL;
    good_count    = 0U;

    last_accepted_period_samples = 0U;

    for (uint8_t k = 0U; k < MA_WINDOW; k++) { ma_buf[k] = 0U; }
    ma_sum = 0U;
    ma_idx = 0U;
    ma_cnt = 0U;

    snap.state          = 0U;
    snap.freq_millihz   = 0U;
    snap.period_samples = 0U;
}

static void process_rise_edge(uint64_t rise_idx)
{
    if (!has_prev_rise)
    {
        prev_rise_idx = rise_idx;
        has_prev_rise = true;
        return;
    }

    uint64_t period = rise_idx - prev_rise_idx;

    if (period < PERIOD_MIN_SAMPLES)
    {
        short_period_rejected++;
        return;
    }

    if (period > PERIOD_MAX_SAMPLES)
    {
        good_count = 0U;
        long_period_rejected++;
        resync_count++;
        prev_rise_idx = rise_idx;
        return;
    }

    prev_rise_idx = rise_idx;
    if (good_count < 0xFFFFFFFFU) good_count++;

    last_accepted_period_samples = (uint32_t)period;

    if (good_count >= REQUIRED_GOOD_COUNT)
    {
        uint64_t fm = (uint64_t)SAMPLE_RATE_HZ * 1000ULL;
        snap.freq_millihz = (uint32_t)((fm + period / 2ULL) / period);
        snap.period_samples = (uint32_t)period;
        snap.state = 1U;
        snap.update_seq++;
        accepted_period_count++;
    }
}

void AppFanFeedback_Process(void)
{
    const uint16_t *buf;
    uint32_t count, seq;
    if (!AppAdcScan_GetCh0Block(&buf, &count, &seq)) return;
    if (seq == last_ch0_seq) return;
    last_ch0_seq = seq;

    raw_min = 4095U; raw_max = 0U;

    for (uint32_t i = 0U; i < count; i += 2U)
    {
        uint16_t raw = buf[i];
        if (raw < raw_min) raw_min = raw;
        if (raw > raw_max) raw_max = raw;

        ma_sum -= ma_buf[ma_idx];
        ma_buf[ma_idx] = raw;
        ma_sum += raw;
        ma_idx = (ma_idx + 1U) % MA_WINDOW;
        if (ma_cnt < MA_WINDOW) { ma_cnt++; total_idx++; continue; }

        uint16_t s = (uint16_t)((ma_sum + MA_WINDOW / 2U) / MA_WINDOW);

        switch (candidate_type)
        {
        case CAND_NONE:
            if (s >= HIGH_THRESHOLD && confirmed_level != CONF_HIGH)
            {
                candidate_type = CAND_HIGH;
                cand_start_idx = total_idx;
                cand_count = 1U;
            }
            else if (s <= LOW_THRESHOLD && confirmed_level != CONF_LOW)
            {
                candidate_type = CAND_LOW;
                cand_start_idx = total_idx;
                cand_count = 1U;
            }
            break;

        case CAND_HIGH:
            if (s >= HIGH_THRESHOLD)
            {
                cand_count++;
                if (cand_count >= MIN_HIGH_SAMPLES)
                {
                    confirmed_level = CONF_HIGH;
                    process_rise_edge(cand_start_idx);
                    candidate_type = CAND_NONE;
                }
            }
            else
            {
                high_cand_rejected++;
                candidate_type = CAND_NONE;
            }
            break;

        case CAND_LOW:
            if (s <= LOW_THRESHOLD)
            {
                cand_count++;
                if (cand_count >= MIN_LOW_SAMPLES)
                {
                    confirmed_level = CONF_LOW;
                    candidate_type = CAND_NONE;
                }
            }
            else
            {
                low_cand_rejected++;
                candidate_type = CAND_NONE;
            }
            break;
        }
        total_idx++;
    }

    snap.raw_min = raw_min;
    snap.raw_max = raw_max;
    snap.rejected_count       = high_cand_rejected + low_cand_rejected;
    snap.accepted_count       = accepted_period_count;
    snap.short_rejected       = short_period_rejected;
    snap.long_rejected        = long_period_rejected;
    snap.resync_count         = resync_count;
    snap.last_accepted_period  = last_accepted_period_samples;
    snap.overrun_count        = AppAdcScan_GetOverrunCount();
}

bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s)
{
    if (s == NULL) return false;
    *s = snap;
    return true;
}
