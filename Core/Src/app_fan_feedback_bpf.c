#include "app_fan_feedback_bpf.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE && \
    !APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE
#error "BPF tach shadow requires the BPF shadow filter"
#endif

#if APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE

#if APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ != 10000U
#error "Fan BPF coefficients must be regenerated when the ADC sample rate changes"
#endif

#if (APP_FAN_FEEDBACK_BPF_LOW_HZ != 20U) || \
    (APP_FAN_FEEDBACK_BPF_HIGH_HZ != 100U)
#error "Fan BPF coefficients must be regenerated when cutoff frequencies change"
#endif

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
#if APP_FAN_FEEDBACK_BPF_TACH_MIN_HZ == 0U
#error "Fan BPF tach minimum frequency must be nonzero"
#endif
#if APP_FAN_FEEDBACK_BPF_TACH_MIN_HZ >= APP_FAN_FEEDBACK_BPF_TACH_MAX_HZ
#error "Fan BPF tach frequency range is invalid"
#endif
#if APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE != 5U
#error "The filtered-tach median helper is intentionally fixed to five periods"
#endif
#if APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS > APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE
#error "Required period count exceeds history size"
#endif
#if APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS < 2U
#error "At least two periods are required"
#endif
#if APP_FAN_FEEDBACK_BPF_TACH_RESYNC_REJECTS == 0U
#error "Tach resynchronization reject count must be nonzero"
#endif
#endif

#define BPF_SECTION_COUNT  2U
#define BPF_WARMUP_SAMPLES \
    ((APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ * APP_FAN_FEEDBACK_BPF_WARMUP_MS) / 1000U)

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
#define TACH_Q16_ONE               65536ULL
#define TACH_TIMEOUT_SAMPLES \
    ((APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ * APP_FAN_FEEDBACK_BPF_TACH_TIMEOUT_MS) / 1000U)
#define TACH_LOW_SIGNAL_BLOCKS_TO_DROP  3U
#endif

typedef struct
{
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} BiquadCoefficients;

typedef struct
{
    float s1;
    float s2;
} BiquadState;

/*
 * scipy.signal.butter(2, [20, 100], btype="bandpass", fs=10000,
 *                     output="sos")
 *
 * scipy SOS denominator convention: 1 + a1*z^-1 + a2*z^-2.
 * The DF2T implementation below therefore uses -a1*y and -a2*y.
 */
static const BiquadCoefficients bpf_coeff[BPF_SECTION_COUNT] =
{
    {
        0.0006098547019f,
        0.001219709404f,
        0.0006098547019f,
       -1.941980720f,
        0.9449790716f
    },
    {
        1.000000000f,
       -2.000000000f,
        1.000000000f,
       -1.985410213f,
        0.9856109619f
    }
};

static BiquadState bpf_state[BPF_SECTION_COUNT];
static AppFanFeedbackBpfStats bpf_stats;
static float block_sum_squares;
static uint8_t initialized;

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
static uint8_t tach_detection_enabled;
static uint8_t tach_previous_output_valid;
static uint8_t tach_has_previous_crossing;
static uint8_t tach_period_history_pos;
static uint8_t tach_inconsistent_streak;
static uint8_t tach_post_short_recovery;
static uint8_t tach_low_signal_block_streak;
static uint8_t tach_last_accept_valid;
static float tach_previous_output;
static uint64_t tach_sample_index;
static uint64_t tach_previous_crossing_q16;
static uint64_t tach_last_accepted_sample_index;
static uint32_t tach_period_history_q16[APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE];
#endif

static float process_biquad(float input,
                            const BiquadCoefficients *coeff,
                            BiquadState *state)
{
    float output = coeff->b0 * input + state->s1;

    state->s1 = coeff->b1 * input - coeff->a1 * output + state->s2;
    state->s2 = coeff->b2 * input - coeff->a2 * output;

    return output;
}

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
static uint32_t saturate_u64_to_u32(uint64_t value)
{
    return (value > UINT32_MAX) ? UINT32_MAX : (uint32_t)value;
}

static uint32_t median_period_q16(void)
{
    uint32_t sorted[APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE];
    uint32_t count = bpf_stats.tach_period_history_count;

    for (uint32_t i = 0U; i < count; i++)
    {
        sorted[i] = tach_period_history_q16[i];
    }

    for (uint32_t i = 1U; i < count; i++)
    {
        uint32_t value = sorted[i];
        uint32_t j = i;

        while ((j > 0U) && (sorted[j - 1U] > value))
        {
            sorted[j] = sorted[j - 1U];
            j--;
        }
        sorted[j] = value;
    }

    if ((count & 1U) != 0U)
    {
        return sorted[count / 2U];
    }

    return (uint32_t)(((uint64_t)sorted[count / 2U - 1U] +
                       (uint64_t)sorted[count / 2U]) / 2ULL);
}

static void reset_tach_current_state(void)
{
    tach_detection_enabled = 0U;
    tach_previous_output_valid = 0U;
    tach_has_previous_crossing = 0U;
    tach_period_history_pos = 0U;
    tach_inconsistent_streak = 0U;
    tach_post_short_recovery = 0U;
    tach_low_signal_block_streak = 0U;
    tach_last_accept_valid = 0U;
    tach_previous_output = 0.0f;
    tach_sample_index = 0ULL;
    tach_previous_crossing_q16 = 0ULL;
    tach_last_accepted_sample_index = 0ULL;
    memset(tach_period_history_q16, 0, sizeof(tach_period_history_q16));

    bpf_stats.tach_shadow_enabled = 1U;
    bpf_stats.tach_signal_present = 0U;
    bpf_stats.tach_armed = 0U;
    bpf_stats.tach_valid = 0U;
    bpf_stats.tach_freq_millihz = 0U;
    bpf_stats.tach_rpm = 0U;
    bpf_stats.tach_period_samples = 0U;
    bpf_stats.tach_hysteresis = APP_FAN_FEEDBACK_BPF_TACH_MIN_HYSTERESIS;
    bpf_stats.tach_rms_estimate = 0.0f;
    bpf_stats.tach_period_history_count = 0U;
    bpf_stats.tach_last_crossing_age_samples = 0U;
}

static void clear_period_history(void)
{
    memset(tach_period_history_q16, 0, sizeof(tach_period_history_q16));
    tach_period_history_pos = 0U;
    tach_inconsistent_streak = 0U;
    tach_post_short_recovery = 0U;
    bpf_stats.tach_period_history_count = 0U;
    bpf_stats.tach_valid = 0U;
    bpf_stats.tach_freq_millihz = 0U;
    bpf_stats.tach_rpm = 0U;
    bpf_stats.tach_period_samples = 0U;
}

static void accept_period(uint32_t period_q16)
{
    uint32_t median_q16;

    tach_period_history_q16[tach_period_history_pos] = period_q16;
    tach_period_history_pos++;
    if (tach_period_history_pos >= APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE)
    {
        tach_period_history_pos = 0U;
    }

    if (bpf_stats.tach_period_history_count <
        APP_FAN_FEEDBACK_BPF_TACH_HISTORY_SIZE)
    {
        bpf_stats.tach_period_history_count++;
    }

    if (bpf_stats.tach_accepted_periods != UINT32_MAX)
    {
        bpf_stats.tach_accepted_periods++;
    }

    tach_last_accepted_sample_index = tach_sample_index;
    tach_last_accept_valid = 1U;
    bpf_stats.tach_last_crossing_age_samples = 0U;

    if (bpf_stats.tach_period_history_count <
        APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS)
    {
        return;
    }

    median_q16 = median_period_q16();
    if (median_q16 == 0U)
    {
        return;
    }

    {
        uint64_t freq_numerator =
            (uint64_t)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ *
            1000ULL * TACH_Q16_ONE;
        uint32_t freq_millihz =
            (uint32_t)((freq_numerator + (uint64_t)median_q16 / 2ULL) /
                       (uint64_t)median_q16);
        uint64_t rpm_numerator =
            (uint64_t)freq_millihz *
            (uint64_t)APP_FAN_FEEDBACK_BPF_TACH_RPM_FACTOR;

        bpf_stats.tach_freq_millihz = freq_millihz;
        bpf_stats.tach_rpm =
            (uint32_t)((rpm_numerator + 500ULL) / 1000ULL);
        bpf_stats.tach_period_samples =
            (uint32_t)(((uint64_t)median_q16 + TACH_Q16_ONE / 2ULL) /
                       TACH_Q16_ONE);
        bpf_stats.tach_valid = 1U;
        if (bpf_stats.tach_update_seq != UINT32_MAX)
        {
            bpf_stats.tach_update_seq++;
        }
    }
}

static void process_crossing(uint64_t crossing_q16)
{
    uint64_t period_q16;
    uint64_t min_period_q16 =
        ((uint64_t)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ * TACH_Q16_ONE) /
        APP_FAN_FEEDBACK_BPF_TACH_MAX_HZ;
    uint64_t max_period_q16 =
        ((uint64_t)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ * TACH_Q16_ONE) /
        APP_FAN_FEEDBACK_BPF_TACH_MIN_HZ;

    if (bpf_stats.tach_rising_crossings != UINT32_MAX)
    {
        bpf_stats.tach_rising_crossings++;
    }

    if (tach_has_previous_crossing == 0U)
    {
        tach_previous_crossing_q16 = crossing_q16;
        tach_has_previous_crossing = 1U;
        return;
    }

    period_q16 = crossing_q16 - tach_previous_crossing_q16;

    if (period_q16 < min_period_q16)
    {
        if (bpf_stats.tach_short_rejected != UINT32_MAX)
        {
            bpf_stats.tach_short_rejected++;
        }
        /*
         * Advance to the observed crossing so a persistent out-of-range high
         * frequency cannot alias into an allowed integer submultiple.  The
         * first following in-range interval is ignored as recovery, which
         * prevents one isolated noise edge from corrupting the history.
         */
        tach_previous_crossing_q16 = crossing_q16;
        tach_post_short_recovery = 1U;
        return;
    }

    tach_previous_crossing_q16 = crossing_q16;

    if (period_q16 > max_period_q16)
    {
        if (bpf_stats.tach_long_rejected != UINT32_MAX)
        {
            bpf_stats.tach_long_rejected++;
        }
        if (bpf_stats.tach_resync_count != UINT32_MAX)
        {
            bpf_stats.tach_resync_count++;
        }
        clear_period_history();
        return;
    }

    if (tach_post_short_recovery != 0U)
    {
        tach_post_short_recovery = 0U;
        if (bpf_stats.tach_post_short_rejected != UINT32_MAX)
        {
            bpf_stats.tach_post_short_rejected++;
        }
        return;
    }

    if (bpf_stats.tach_period_history_count >=
        APP_FAN_FEEDBACK_BPF_TACH_REQUIRED_PERIODS)
    {
        uint32_t reference_q16 = median_period_q16();
        uint64_t difference = (period_q16 >= reference_q16)
            ? period_q16 - (uint64_t)reference_q16
            : (uint64_t)reference_q16 - period_q16;

        if ((difference * 100ULL) >
            ((uint64_t)reference_q16 *
             APP_FAN_FEEDBACK_BPF_TACH_PERIOD_TOLERANCE_PERCENT))
        {
            if (bpf_stats.tach_inconsistent_rejected != UINT32_MAX)
            {
                bpf_stats.tach_inconsistent_rejected++;
            }
            if (tach_inconsistent_streak < UINT8_MAX)
            {
                tach_inconsistent_streak++;
            }

            if (tach_inconsistent_streak >=
                APP_FAN_FEEDBACK_BPF_TACH_RESYNC_REJECTS)
            {
                if (bpf_stats.tach_resync_count != UINT32_MAX)
                {
                    bpf_stats.tach_resync_count++;
                }
                clear_period_history();
                accept_period((uint32_t)period_q16);
            }
            return;
        }
    }

    tach_inconsistent_streak = 0U;
    accept_period((uint32_t)period_q16);
}

static void process_tach_sample(float output)
{
    if (tach_detection_enabled != 0U)
    {
        float hysteresis = bpf_stats.tach_hysteresis;

        if ((bpf_stats.tach_armed == 0U) && (output <= -hysteresis))
        {
            bpf_stats.tach_armed = 1U;
        }

        if ((bpf_stats.tach_armed != 0U) &&
            (tach_previous_output_valid != 0U) &&
            (tach_previous_output < 0.0f) &&
            (output >= 0.0f))
        {
            float denominator = output - tach_previous_output;
            uint64_t crossing_q16;

            if ((denominator > 0.0f) && (tach_sample_index > 0ULL))
            {
                float fraction = -tach_previous_output / denominator;
                uint32_t fraction_q16;

                if (fraction < 0.0f) fraction = 0.0f;
                if (fraction > 0.9999847412f) fraction = 0.9999847412f;
                fraction_q16 = (uint32_t)(fraction * 65536.0f + 0.5f);

                crossing_q16 =
                    ((tach_sample_index - 1ULL) << 16) + fraction_q16;
                process_crossing(crossing_q16);
            }

            bpf_stats.tach_armed = 0U;
        }
    }
    else
    {
        bpf_stats.tach_armed = 0U;
    }

    tach_previous_output = output;
    tach_previous_output_valid = 1U;
    tach_sample_index++;
}

static void update_tach_block_state(void)
{
    if ((bpf_stats.warmed_up == 0U) ||
        (bpf_stats.block_samples == 0U))
    {
        bpf_stats.tach_signal_present = 0U;
        tach_detection_enabled = 0U;
        bpf_stats.tach_armed = 0U;
        return;
    }

    if (bpf_stats.tach_rms_estimate <= 0.0f)
    {
        bpf_stats.tach_rms_estimate = bpf_stats.block_rms;
    }
    else if (bpf_stats.block_rms < bpf_stats.tach_rms_estimate)
    {
        bpf_stats.tach_rms_estimate +=
            0.50f * (bpf_stats.block_rms - bpf_stats.tach_rms_estimate);
    }
    else
    {
        bpf_stats.tach_rms_estimate +=
            0.25f * (bpf_stats.block_rms - bpf_stats.tach_rms_estimate);
    }

    bpf_stats.tach_hysteresis =
        bpf_stats.tach_rms_estimate *
        ((float)APP_FAN_FEEDBACK_BPF_TACH_HYST_RMS_PERCENT / 100.0f);
    if (bpf_stats.tach_hysteresis <
        APP_FAN_FEEDBACK_BPF_TACH_MIN_HYSTERESIS)
    {
        bpf_stats.tach_hysteresis =
            APP_FAN_FEEDBACK_BPF_TACH_MIN_HYSTERESIS;
    }

    if (bpf_stats.block_rms >= APP_FAN_FEEDBACK_BPF_TACH_MIN_SIGNAL_RMS)
    {
        tach_low_signal_block_streak = 0U;
        bpf_stats.tach_signal_present = 1U;
        tach_detection_enabled = 1U;
    }
    else
    {
        if (tach_low_signal_block_streak < UINT8_MAX)
        {
            tach_low_signal_block_streak++;
        }

        if (tach_low_signal_block_streak >= TACH_LOW_SIGNAL_BLOCKS_TO_DROP)
        {
            bpf_stats.tach_signal_present = 0U;
            tach_detection_enabled = 0U;
            bpf_stats.tach_armed = 0U;
            tach_has_previous_crossing = 0U;
        }
    }

    if (tach_last_accept_valid != 0U)
    {
        uint64_t age = tach_sample_index - tach_last_accepted_sample_index;
        bpf_stats.tach_last_crossing_age_samples = saturate_u64_to_u32(age);

        if ((age >= TACH_TIMEOUT_SAMPLES) &&
            (bpf_stats.tach_valid != 0U))
        {
            if (bpf_stats.tach_timeout_count != UINT32_MAX)
            {
                bpf_stats.tach_timeout_count++;
            }
            clear_period_history();
            tach_has_previous_crossing = 0U;
            tach_last_accept_valid = 0U;
        }
    }
}
#endif

static void reset_filter_state(void)
{
    memset(bpf_state, 0, sizeof(bpf_state));
    bpf_stats.warmed_up = 0U;
    bpf_stats.warmup_remaining_samples = BPF_WARMUP_SAMPLES;
    bpf_stats.output_last = 0.0f;
    bpf_stats.block_min = 0.0f;
    bpf_stats.block_max = 0.0f;
    bpf_stats.block_peak_to_peak = 0.0f;
    bpf_stats.block_rms = 0.0f;
    bpf_stats.block_samples = 0U;
    block_sum_squares = 0.0f;
#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
    reset_tach_current_state();
#endif
}

void AppFanFeedbackBpf_Init(void)
{
    memset(&bpf_stats, 0, sizeof(bpf_stats));
    bpf_stats.enabled = 1U;
    initialized = 1U;
    reset_filter_state();
}

void AppFanFeedbackBpf_Reset(void)
{
    if (initialized == 0U)
    {
        AppFanFeedbackBpf_Init();
    }

    if (bpf_stats.reset_count != UINT32_MAX)
    {
        bpf_stats.reset_count++;
    }
    reset_filter_state();
}

void AppFanFeedbackBpf_BeginBlock(void)
{
    if (initialized == 0U)
    {
        AppFanFeedbackBpf_Init();
    }

    bpf_stats.block_min = FLT_MAX;
    bpf_stats.block_max = -FLT_MAX;
    bpf_stats.block_peak_to_peak = 0.0f;
    bpf_stats.block_rms = 0.0f;
    bpf_stats.block_samples = 0U;
    block_sum_squares = 0.0f;
}

void AppFanFeedbackBpf_ProcessSample(uint16_t sample)
{
    float output = (float)sample;

    if (initialized == 0U)
    {
        AppFanFeedbackBpf_Init();
        AppFanFeedbackBpf_BeginBlock();
    }

    for (uint32_t section = 0U; section < BPF_SECTION_COUNT; section++)
    {
        output = process_biquad(output, &bpf_coeff[section],
                                &bpf_state[section]);
    }

    if (!isfinite(output))
    {
        if (bpf_stats.nonfinite_count != UINT32_MAX)
        {
            bpf_stats.nonfinite_count++;
        }
        AppFanFeedbackBpf_Reset();
        return;
    }

    bpf_stats.output_last = output;
    if (bpf_stats.processed_samples != UINT32_MAX)
    {
        bpf_stats.processed_samples++;
    }

    if (bpf_stats.warmup_remaining_samples > 0U)
    {
        bpf_stats.warmup_remaining_samples--;
        if (bpf_stats.warmup_remaining_samples == 0U)
        {
            bpf_stats.warmed_up = 1U;
        }
    }

    if (output < bpf_stats.block_min)
    {
        bpf_stats.block_min = output;
    }
    if (output > bpf_stats.block_max)
    {
        bpf_stats.block_max = output;
    }

    block_sum_squares += output * output;
    if (bpf_stats.block_samples != UINT32_MAX)
    {
        bpf_stats.block_samples++;
    }

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
    process_tach_sample(output);
#endif
}

void AppFanFeedbackBpf_EndBlock(void)
{
    if ((initialized == 0U) || (bpf_stats.block_samples == 0U))
    {
        bpf_stats.block_min = 0.0f;
        bpf_stats.block_max = 0.0f;
        bpf_stats.block_peak_to_peak = 0.0f;
        bpf_stats.block_rms = 0.0f;
        return;
    }

    bpf_stats.block_peak_to_peak = bpf_stats.block_max - bpf_stats.block_min;
    bpf_stats.block_rms = sqrtf(block_sum_squares /
                                (float)bpf_stats.block_samples);

    if (!isfinite(bpf_stats.block_rms))
    {
        if (bpf_stats.nonfinite_count != UINT32_MAX)
        {
            bpf_stats.nonfinite_count++;
        }
        AppFanFeedbackBpf_Reset();
        return;
    }

#if APP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE
    update_tach_block_state();
#endif
}

bool AppFanFeedbackBpf_GetStats(AppFanFeedbackBpfStats *stats)
{
    if (stats == NULL)
    {
        return false;
    }

    if (initialized == 0U)
    {
        AppFanFeedbackBpf_Init();
    }

    *stats = bpf_stats;
    return true;
}

#else

void AppFanFeedbackBpf_Init(void)
{
}

void AppFanFeedbackBpf_Reset(void)
{
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

    memset(stats, 0, sizeof(*stats));
    return true;
}

#endif
