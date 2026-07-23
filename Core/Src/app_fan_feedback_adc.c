#include "app_fan_feedback_adc.h"
#include "app_adc_scan.h"
#include <stddef.h>

#define SAMPLE_RATE_HZ             10000U
#define MA_WINDOW                  10U
#define MIN_HIGH_SAMPLES           20U
#define MIN_LOW_SAMPLES            5U

#define FG_MIN_FREQ_HZ             10U
#define FG_MAX_FREQ_HZ             120U
#define PERIOD_MIN_SAMPLES         ((SAMPLE_RATE_HZ) / (FG_MAX_FREQ_HZ))
#define PERIOD_MAX_SAMPLES         ((SAMPLE_RATE_HZ) / (FG_MIN_FREQ_HZ))
#define REQUIRED_GOOD_COUNT        3U

#define MIN_VALID_SPAN             400U
#define ENV_EMA_SHIFT              2U
#define LOW_FRAC                   35U
#define HIGH_FRAC                  65U
#define DENOM                      100U

#define TACH_TIMEOUT_MS_DEFAULT    500U

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

static bool     envelope_valid;
static uint16_t low_envelope;
static uint16_t high_envelope;
static uint16_t curr_low_threshold;
static uint16_t curr_high_threshold;

static uint32_t last_valid_update_tick;
static uint32_t tach_timeout_accum_ms;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
static uint64_t diag_raw_sum;
static uint32_t diag_raw_count;
static uint64_t diag_filt_sum;
static uint32_t diag_filt_count;
static uint16_t diag_filt_min;
static uint16_t diag_filt_max;
static uint32_t diag_above_high;
static uint32_t diag_below_low;
static uint32_t diag_cur_above;
static uint32_t diag_max_above;
static uint32_t diag_cur_below;
static uint32_t diag_max_below;
static uint32_t diag_rise_count;
static uint32_t diag_fall_count;
static uint32_t diag_high_cand_started;
static uint32_t diag_low_cand_started;

static void diag_reset_window(void)
{
    diag_raw_sum   = 0ULL;
    diag_raw_count = 0U;
    diag_filt_sum  = 0ULL;
    diag_filt_count = 0U;
    diag_filt_min  = 4095U;
    diag_filt_max  = 0U;
    diag_above_high = 0U;
    diag_below_low  = 0U;
    diag_cur_above  = 0U;
    diag_max_above  = 0U;
    diag_cur_below  = 0U;
    diag_max_below  = 0U;
    diag_rise_count = 0U;
    diag_fall_count = 0U;
    diag_high_cand_started = 0U;
    diag_low_cand_started  = 0U;
}

static void diag_record_raw(uint16_t raw)
{
    diag_raw_sum += raw;
    diag_raw_count++;
}

static void diag_record_filtered(uint16_t s, CandidateType prev_cand,
                                  CandidateType new_cand)
{
    diag_filt_sum += s;
    diag_filt_count++;
    if (s < diag_filt_min) diag_filt_min = s;
    if (s > diag_filt_max) diag_filt_max = s;

    if (s >= curr_high_threshold)
    {
        diag_above_high++;
        diag_cur_above++;
        if (diag_cur_above > diag_max_above)
            diag_max_above = diag_cur_above;
        diag_cur_below = 0U;
    }
    else
    {
        diag_cur_above = 0U;
    }

    if (s <= curr_low_threshold)
    {
        diag_below_low++;
        diag_cur_below++;
        if (diag_cur_below > diag_max_below)
            diag_max_below = diag_cur_below;
        diag_cur_above = 0U;
    }
    else
    {
        diag_cur_below = 0U;
    }

    if (prev_cand == CAND_NONE && new_cand == CAND_HIGH)
        diag_high_cand_started++;
    if (prev_cand == CAND_NONE && new_cand == CAND_LOW)
        diag_low_cand_started++;
}

static void diag_commit_window(void)
{
    if (diag_raw_count > 0U)
        snap.raw_average =
            (uint16_t)((diag_raw_sum + diag_raw_count / 2U) / diag_raw_count);
    else
        snap.raw_average = 0U;

    snap.raw_span = (raw_max >= raw_min)
        ? (uint16_t)(raw_max - raw_min) : 0U;

    if (diag_filt_count > 0U)
        snap.filtered_average =
            (uint16_t)((diag_filt_sum + diag_filt_count / 2U) / diag_filt_count);
    else
        snap.filtered_average = 0U;

    snap.filtered_min     = diag_filt_min;
    snap.filtered_max     = diag_filt_max;
    snap.filtered_span    = (diag_filt_max >= diag_filt_min)
        ? (uint16_t)(diag_filt_max - diag_filt_min) : 0U;

    snap.samples_at_or_above_high   = diag_above_high;
    snap.samples_at_or_below_low    = diag_below_low;
    snap.max_consecutive_above_high = diag_max_above;
    snap.max_consecutive_below_low  = diag_max_below;

    snap.raw_rising_edge_count  = diag_rise_count;
    snap.raw_falling_edge_count = diag_fall_count;
    snap.high_candidate_started = diag_high_cand_started;
    snap.low_candidate_started  = diag_low_cand_started;

    snap.current_candidate_state  = (uint8_t)candidate_type;
    snap.current_confirmed_level  = (uint8_t)confirmed_level;
    snap.current_candidate_count  = cand_count;

    snap.diag_window_time_ms =
        (uint32_t)((diag_filt_count * 1000ULL + SAMPLE_RATE_HZ / 2ULL) / SAMPLE_RATE_HZ);

    snap.high_candidate_rejected_count = high_cand_rejected;
    snap.low_candidate_rejected_count  = low_cand_rejected;
    snap.accepted_period_count         = accepted_period_count;
    snap.last_valid_update_age_ms      =
        (snap.state == 1U && last_valid_update_tick != 0U)
        ? (uint32_t)(tach_timeout_accum_ms) : 0U;

    snap.current_low_threshold   = curr_low_threshold;
    snap.current_high_threshold  = curr_high_threshold;
    snap.current_low_envelope    = low_envelope;
    snap.current_high_envelope   = high_envelope;
    snap.current_span            =
        (high_envelope >= low_envelope)
        ? (uint16_t)(high_envelope - low_envelope) : 0U;
    snap.envelope_valid          = envelope_valid ? 1U : 0U;
}
#endif

static void update_envelope(uint16_t flt_min, uint16_t flt_max,
                             uint32_t flt_count)
{
    if (flt_count == 0U) return;

    if (!envelope_valid)
    {
        low_envelope  = flt_min;
        high_envelope = flt_max;
        envelope_valid = true;
    }
    else
    {
        int32_t diff;

        diff = (int32_t)flt_min - (int32_t)low_envelope;
        low_envelope = (uint16_t)((int32_t)low_envelope + (diff >> ENV_EMA_SHIFT));

        diff = (int32_t)flt_max - (int32_t)high_envelope;
        high_envelope = (uint16_t)((int32_t)high_envelope + (diff >> ENV_EMA_SHIFT));
    }

    {
        uint32_t span = (high_envelope >= low_envelope)
            ? (uint32_t)(high_envelope - low_envelope) : 0U;

        if (span < MIN_VALID_SPAN)
        {
            envelope_valid = false;
            curr_low_threshold  = 0U;
            curr_high_threshold = 0U;
        }
        else
        {
            uint32_t lo = (uint32_t)low_envelope + (span * LOW_FRAC) / DENOM;
            uint32_t hi = (uint32_t)low_envelope + (span * HIGH_FRAC) / DENOM;

            if (lo > 4095U) lo = 4095U;
            if (hi > 4095U) hi = 4095U;

            if (lo < hi)
            {
                curr_low_threshold  = (uint16_t)lo;
                curr_high_threshold = (uint16_t)hi;
                envelope_valid = true;
            }
            else
            {
                envelope_valid = false;
                curr_low_threshold  = 0U;
                curr_high_threshold = 0U;
            }
        }
    }
}

static void reset_state_machine(void)
{
    confirmed_level = CONF_NONE;
    candidate_type  = CAND_NONE;
    cand_start_idx  = 0U;
    cand_count      = 0U;
    has_prev_rise   = false;
    prev_rise_idx   = 0ULL;
    good_count      = 0U;
}

void AppFanFeedback_ResetMeasurement(void)
{
    reset_state_machine();

    last_accepted_period_samples = 0U;

    for (uint8_t k = 0U; k < MA_WINDOW; k++) { ma_buf[k] = 0U; }
    ma_sum = 0U;
    ma_idx = 0U;
    ma_cnt = 0U;

    envelope_valid      = false;
    low_envelope        = 0U;
    high_envelope       = 0U;
    curr_low_threshold  = 0U;
    curr_high_threshold = 0U;

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
        last_valid_update_tick = tach_timeout_accum_ms;
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
    uint32_t flt_samples = 0U;
    {
        uint16_t flt_min = 4095U, flt_max = 0U;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    diag_reset_window();
#endif

        for (uint32_t i = 0U; i < count; i += 2U)
        {
            uint16_t raw = buf[i];
            if (raw < raw_min) raw_min = raw;
            if (raw > raw_max) raw_max = raw;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
            diag_record_raw(raw);
#endif
            ma_sum -= ma_buf[ma_idx];
            ma_buf[ma_idx] = raw;
            ma_sum += raw;
            ma_idx = (ma_idx + 1U) % MA_WINDOW;
            if (ma_cnt < MA_WINDOW) { ma_cnt++; total_idx++; continue; }

            uint16_t s = (uint16_t)((ma_sum + MA_WINDOW / 2U) / MA_WINDOW);

            if (s < flt_min) flt_min = s;
            if (s > flt_max) flt_max = s;
            flt_samples++;

            if (envelope_valid)
            {
#if APP_FAN_FEEDBACK_DIAG_ENABLED
                CandidateType prev_cand = candidate_type;
#endif
                switch (candidate_type)
                {
                case CAND_NONE:
                    if (s >= curr_high_threshold && confirmed_level != CONF_HIGH)
                    {
                        candidate_type = CAND_HIGH;
                        cand_start_idx = total_idx;
                        cand_count = 1U;
                    }
                    else if (s <= curr_low_threshold &&
                             confirmed_level != CONF_LOW)
                    {
                        candidate_type = CAND_LOW;
                        cand_start_idx = total_idx;
                        cand_count = 1U;
                    }
                    break;

                case CAND_HIGH:
                    if (s >= curr_high_threshold)
                    {
                        cand_count++;
                        if (cand_count >= MIN_HIGH_SAMPLES)
                        {
                            confirmed_level = CONF_HIGH;
                            process_rise_edge(cand_start_idx);
#if APP_FAN_FEEDBACK_DIAG_ENABLED
                            diag_rise_count++;
#endif
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
                    if (s <= curr_low_threshold)
                    {
                        cand_count++;
                        if (cand_count >= MIN_LOW_SAMPLES)
                        {
                            confirmed_level = CONF_LOW;
#if APP_FAN_FEEDBACK_DIAG_ENABLED
                            diag_fall_count++;
#endif
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
#if APP_FAN_FEEDBACK_DIAG_ENABLED
                diag_record_filtered(s, prev_cand, candidate_type);
#endif
            }
            total_idx++;
        }

        update_envelope(flt_min, flt_max, flt_samples);

        if (!envelope_valid)
        {
            reset_state_machine();
        }
    }

    {
        uint32_t now = tach_timeout_accum_ms;
        if (snap.state == 1U &&
            (now - last_valid_update_tick) >= TACH_TIMEOUT_MS_DEFAULT)
        {
            snap.state        = 0U;
            snap.freq_millihz = 0U;
        }
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

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    diag_commit_window();
#endif

    tach_timeout_accum_ms +=
        (uint32_t)((flt_samples * 1000ULL + SAMPLE_RATE_HZ - 1ULL)
                    / SAMPLE_RATE_HZ);
}

bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s)
{
    if (s == NULL) return false;
    *s = snap;
    return true;
}
