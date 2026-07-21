#include "app_fan_feedback_adc.h"
#include "app_adc_scan.h"
#include <stddef.h>

#define SAMPLE_RATE_HZ      10000U
#define MA_WINDOW           10U
#define LOW_THRESHOLD       900U
#define HIGH_THRESHOLD      1800U
#define MIN_HIGH_SAMPLES    20U
#define MIN_LOW_SAMPLES     5U
#define TIMEOUT_MS          500U

typedef enum { ST_IDLE, ST_LOW, ST_HIGH } LevelState;

static AppFanFeedbackSnapshot snap;
static uint32_t last_ok_tick;
static uint32_t last_ch0_seq;
static uint32_t total_idx;
static LevelState cur_level;
static uint32_t level_start_idx;
static bool     cur_level_valid;
static uint64_t prev_rise_idx;
static bool     has_prev_rise;
static uint32_t good_count;
static uint32_t raw_min, raw_max;
static uint32_t rejected_count;

static uint16_t ma_buf[MA_WINDOW];
static uint32_t ma_sum;
static uint8_t  ma_idx;
static uint8_t  ma_cnt;

static void process_level_change(uint64_t rise_idx)
{
    if (has_prev_rise)
    {
        uint64_t period = rise_idx - prev_rise_idx;
        if (period >= 100U && period <= 20000U)
        {
            good_count++;
            if (good_count >= 3U)
            {
                uint64_t fm = (uint64_t)SAMPLE_RATE_HZ * 1000ULL;
                snap.freq_millihz = (uint32_t)((fm + period / 2ULL) / period);
                snap.period_samples = (uint32_t)period;
                snap.age_ms = 0U;
                snap.state = 1U;
                last_ok_tick = 0U;
            }
        }
        else good_count = 0U;
    }
    prev_rise_idx = rise_idx;
    has_prev_rise = true;
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
        if (ma_cnt < MA_WINDOW) { ma_cnt++; continue; }

        uint16_t s = (uint16_t)((ma_sum + MA_WINDOW / 2U) / MA_WINDOW);

        LevelState new_level;
        if (s >= HIGH_THRESHOLD)      new_level = ST_HIGH;
        else if (s <= LOW_THRESHOLD)  new_level = ST_LOW;
        else                          new_level = cur_level;

        if (new_level != cur_level)
        {
            uint32_t dur = (total_idx >= level_start_idx) ? (total_idx - level_start_idx) : 0U;
            bool is_short = false;
            if (cur_level_valid)
            {
                if ((cur_level == ST_HIGH  && dur < MIN_HIGH_SAMPLES) ||
                    (cur_level == ST_LOW   && dur < MIN_LOW_SAMPLES))
                    is_short = true;
            }

            if (!is_short)
            {
                if (new_level == ST_HIGH) process_level_change(total_idx);
            }
            else
            {
                rejected_count++;
            }

            cur_level       = new_level;
            level_start_idx = total_idx;
            cur_level_valid = true;
        }
        total_idx++;
    }

    snap.raw_min = raw_min;
    snap.raw_max = raw_max;
    snap.rejected_count = rejected_count;

    if (snap.state == 0U || good_count < 3U)
    {
        snap.freq_millihz = 0U;
        snap.state = 0U;
    }
}

bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s)
{
    if (s == NULL) return false;
    *s = snap;
    return true;
}
