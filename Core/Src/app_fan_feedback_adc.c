#include "app_fan_feedback_adc.h"
#include "app_adc_scan.h"
#include "app_fan_feedback_bpf.h"
#include <stddef.h>

#define SAMPLE_RATE_HZ             10000U

#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE && \
    (SAMPLE_RATE_HZ != APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ)
#error "Fan feedback and shadow BPF sample rates must match"
#endif

#if (APP_FAN_FEEDBACK_OUTPUT_MODE != APP_FAN_FEEDBACK_OUTPUT_MODE_LEGACY) && \
    (APP_FAN_FEEDBACK_OUTPUT_MODE != APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY)
#error "Invalid fan feedback output mode"
#endif

#if (APP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE != 0U) && \
    (APP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE != 1U)
#error "Legacy fan tach fallback must be 0 or 1"
#endif

#if (APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY) && \
    (!APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE || \
     !APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE)
#error "BPF-primary fan feedback requires the BPF filter and tachometer"
#endif
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

/* Legacy detector state/diagnostics. */
static AppFanFeedbackSnapshot snap;

/* Official feedback after stage-3 source arbitration. */
static AppFanFeedbackSnapshot official_snap;
static uint32_t official_update_seq;
static uint32_t selected_source_seq;
static uint32_t source_switch_count;
static uint8_t  selected_source;

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

static void invalidate_official_feedback(void)
{
    official_snap = snap;
    official_snap.state = 0U;
    official_snap.freq_millihz = 0U;
    official_snap.period_samples = 0U;
    official_snap.update_seq = official_update_seq;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    official_snap.active_source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE;
    official_snap.bpf_tach_valid = 0U;
    official_snap.legacy_tach_valid = 0U;
    official_snap.legacy_fallback_active = 0U;
    official_snap.bpf_freq_millihz = 0U;
    official_snap.legacy_freq_millihz = 0U;
    official_snap.source_switch_count = source_switch_count;
#endif

    selected_source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE;
    selected_source_seq = 0U;
}

static void publish_official_feedback(void)
{
    AppFanFeedbackSnapshot next = snap;
    AppFanFeedbackBpfStats bpf;
    bool bpf_stats_ok = AppFanFeedbackBpf_GetStats(&bpf);
    bool bpf_valid = bpf_stats_ok && (bpf.tach_valid != 0U);
    bool legacy_valid = (snap.state == 1U);
    uint8_t source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE;
    uint32_t source_seq = 0U;

#if APP_FAN_FEEDBACK_OUTPUT_MODE == APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY
    if (bpf_valid)
    {
        source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_BPF;
        source_seq = bpf.tach_update_seq;
        next.state = 1U;
        next.freq_millihz = bpf.tach_freq_millihz;
        next.period_samples = bpf.tach_period_samples;
    }
#if APP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE
    else if (legacy_valid)
    {
        source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY;
        source_seq = snap.update_seq;
        next.state = 1U;
        next.freq_millihz = snap.freq_millihz;
        next.period_samples = snap.period_samples;
    }
#endif
#else
    if (legacy_valid)
    {
        source = APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY;
        source_seq = snap.update_seq;
        next.state = 1U;
        next.freq_millihz = snap.freq_millihz;
        next.period_samples = snap.period_samples;
    }
#endif

    if (source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE)
    {
        next.state = 0U;
        next.freq_millihz = 0U;
        next.period_samples = 0U;
    }

    if ((source != APP_FAN_FEEDBACK_ACTIVE_SOURCE_NONE) &&
        ((source != selected_source) ||
         (source_seq != selected_source_seq)))
    {
        official_update_seq++;
    }

    if (source != selected_source)
    {
        if (source_switch_count != UINT32_MAX)
        {
            source_switch_count++;
        }
    }

    selected_source = source;
    selected_source_seq = source_seq;
    next.update_seq = official_update_seq;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    next.active_source = source;
    next.bpf_tach_valid = bpf_valid ? 1U : 0U;
    next.legacy_tach_valid = legacy_valid ? 1U : 0U;
    next.legacy_fallback_active =
        (source == APP_FAN_FEEDBACK_ACTIVE_SOURCE_LEGACY &&
         APP_FAN_FEEDBACK_OUTPUT_MODE ==
             APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY)
        ? 1U : 0U;
    next.bpf_freq_millihz = bpf_valid ? bpf.tach_freq_millihz : 0U;
    next.legacy_freq_millihz = legacy_valid ? snap.freq_millihz : 0U;
    next.source_switch_count = source_switch_count;
#endif

    official_snap = next;
}

static void reset_legacy_measurement(bool reset_shadow_bpf)
{
    reset_state_machine();
#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
    if (reset_shadow_bpf)
    {
        AppFanFeedbackBpf_Reset();
    }
#else
    (void)reset_shadow_bpf;
#endif

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

    if (reset_shadow_bpf)
    {
        invalidate_official_feedback();
    }
}

void AppFanFeedback_ResetMeasurement(void)
{
    reset_legacy_measurement(true);
}

void AppFanFeedback_ReacquireAfterDutyChange(void)
{
    /*
     * Keep the filtered tachometer, official source sequence and last result
     * intact across small PWM adjustments.  The legacy detector is reset and
     * remains available as a fallback after it reacquires.
     */
    reset_legacy_measurement(false);
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
#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
    AppFanFeedbackBpf_BeginBlock();
#endif
    {
        uint16_t flt_min = 4095U, flt_max = 0U;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    diag_reset_window();
#endif

        for (uint32_t i = 0U; i < count; i += 2U)
        {
            uint16_t raw = buf[i];
#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
            AppFanFeedbackBpf_ProcessSample(raw);
#endif
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

#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
        AppFanFeedbackBpf_EndBlock();
#endif
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

    publish_official_feedback();
}

bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s)
{
    if (s == NULL) return false;
    *s = official_snap;
    return true;
}
