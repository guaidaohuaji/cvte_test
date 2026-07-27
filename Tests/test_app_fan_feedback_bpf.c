#include "app_fan_feedback_bpf.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_PI 3.14159265358979323846f
#define BLOCK_SAMPLES 512U

static float run_sine(float frequency_hz, float amplitude, float offset,
                      float duration_s, float measure_last_s)
{
    uint32_t total_samples = (uint32_t)(duration_s *
                                        APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    uint32_t measure_start = total_samples -
        (uint32_t)(measure_last_s * APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    double sum_squares = 0.0;
    uint32_t measured_samples = 0U;

    AppFanFeedbackBpf_Reset();

    for (uint32_t base = 0U; base < total_samples; base += BLOCK_SAMPLES)
    {
        uint32_t block_count = total_samples - base;
        if (block_count > BLOCK_SAMPLES)
        {
            block_count = BLOCK_SAMPLES;
        }

        AppFanFeedbackBpf_BeginBlock();
        for (uint32_t i = 0U; i < block_count; i++)
        {
            uint32_t sample_index = base + i;
            float phase = 2.0f * TEST_PI * frequency_hz *
                          (float)sample_index /
                          (float)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ;
            float input = offset + amplitude * sinf(phase);
            if (input < 0.0f) input = 0.0f;
            if (input > 4095.0f) input = 4095.0f;
            AppFanFeedbackBpf_ProcessSample((uint16_t)lrintf(input));
        }
        AppFanFeedbackBpf_EndBlock();

        if ((base + block_count) > measure_start)
        {
            AppFanFeedbackBpfStats stats;
            assert(AppFanFeedbackBpf_GetStats(&stats));
            sum_squares += (double)stats.block_rms *
                           (double)stats.block_rms *
                           (double)stats.block_samples;
            measured_samples += stats.block_samples;
        }
    }

    assert(measured_samples > 0U);
    return (float)sqrt(sum_squares / (double)measured_samples);
}

static float run_dc(uint16_t level, float duration_s, float measure_last_s)
{
    uint32_t total_samples = (uint32_t)(duration_s *
                                        APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    uint32_t measure_start = total_samples -
        (uint32_t)(measure_last_s * APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    double sum_squares = 0.0;
    uint32_t measured_samples = 0U;

    AppFanFeedbackBpf_Reset();

    for (uint32_t base = 0U; base < total_samples; base += BLOCK_SAMPLES)
    {
        uint32_t block_count = total_samples - base;
        if (block_count > BLOCK_SAMPLES)
        {
            block_count = BLOCK_SAMPLES;
        }

        AppFanFeedbackBpf_BeginBlock();
        for (uint32_t i = 0U; i < block_count; i++)
        {
            AppFanFeedbackBpf_ProcessSample(level);
        }
        AppFanFeedbackBpf_EndBlock();

        if ((base + block_count) > measure_start)
        {
            AppFanFeedbackBpfStats stats;
            assert(AppFanFeedbackBpf_GetStats(&stats));
            sum_squares += (double)stats.block_rms *
                           (double)stats.block_rms *
                           (double)stats.block_samples;
            measured_samples += stats.block_samples;
        }
    }

    return (float)sqrt(sum_squares / (double)measured_samples);
}


static float run_composite(float duration_s, float measure_last_s)
{
    uint32_t total_samples = (uint32_t)(duration_s *
                                        APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    uint32_t measure_start = total_samples -
        (uint32_t)(measure_last_s * APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    double sum_squares = 0.0;
    uint32_t measured_samples = 0U;

    AppFanFeedbackBpf_Reset();

    for (uint32_t base = 0U; base < total_samples; base += BLOCK_SAMPLES)
    {
        uint32_t block_count = total_samples - base;
        if (block_count > BLOCK_SAMPLES) block_count = BLOCK_SAMPLES;

        AppFanFeedbackBpf_BeginBlock();
        for (uint32_t i = 0U; i < block_count; i++)
        {
            uint32_t n = base + i;
            float t = (float)n / (float)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ;
            float input = 2048.0f
                        + 500.0f * sinf(2.0f * TEST_PI * 50.0f * t)
                        + 500.0f * sinf(2.0f * TEST_PI * 1000.0f * t);
            AppFanFeedbackBpf_ProcessSample((uint16_t)lrintf(input));
        }
        AppFanFeedbackBpf_EndBlock();

        if ((base + block_count) > measure_start)
        {
            AppFanFeedbackBpfStats stats;
            assert(AppFanFeedbackBpf_GetStats(&stats));
            sum_squares += (double)stats.block_rms * stats.block_rms *
                           stats.block_samples;
            measured_samples += stats.block_samples;
        }
    }

    return (float)sqrt(sum_squares / (double)measured_samples);
}

static float run_baseline_drift(float duration_s, float measure_last_s)
{
    uint32_t total_samples = (uint32_t)(duration_s *
                                        APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    uint32_t measure_start = total_samples -
        (uint32_t)(measure_last_s * APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);
    double sum_squares = 0.0;
    uint32_t measured_samples = 0U;

    AppFanFeedbackBpf_Reset();

    for (uint32_t base = 0U; base < total_samples; base += BLOCK_SAMPLES)
    {
        uint32_t block_count = total_samples - base;
        if (block_count > BLOCK_SAMPLES) block_count = BLOCK_SAMPLES;

        AppFanFeedbackBpf_BeginBlock();
        for (uint32_t i = 0U; i < block_count; i++)
        {
            uint32_t n = base + i;
            float t = (float)n / (float)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ;
            float input = 2048.0f
                        + 800.0f * sinf(2.0f * TEST_PI * 33.333333f * t)
                        + 400.0f * sinf(2.0f * TEST_PI * 1.0f * t);
            AppFanFeedbackBpf_ProcessSample((uint16_t)lrintf(input));
        }
        AppFanFeedbackBpf_EndBlock();

        if ((base + block_count) > measure_start)
        {
            AppFanFeedbackBpfStats stats;
            assert(AppFanFeedbackBpf_GetStats(&stats));
            sum_squares += (double)stats.block_rms * stats.block_rms *
                           stats.block_samples;
            measured_samples += stats.block_samples;
        }
    }

    return (float)sqrt(sum_squares / (double)measured_samples);
}


static void feed_tach_segment(uint64_t *sample_index,
                              float frequency_hz,
                              float amplitude,
                              float offset,
                              float duration_s,
                              float drift_amplitude,
                              float high_frequency_amplitude)
{
    uint32_t total_samples = (uint32_t)(duration_s *
                                        APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ);

    for (uint32_t base = 0U; base < total_samples; base += BLOCK_SAMPLES)
    {
        uint32_t block_count = total_samples - base;
        if (block_count > BLOCK_SAMPLES) block_count = BLOCK_SAMPLES;

        AppFanFeedbackBpf_BeginBlock();
        for (uint32_t i = 0U; i < block_count; i++)
        {
            float t = (float)(*sample_index) /
                      (float)APP_FAN_FEEDBACK_BPF_SAMPLE_RATE_HZ;
            float input = offset;

            if (frequency_hz > 0.0f)
            {
                input += amplitude *
                    sinf(2.0f * TEST_PI * frequency_hz * t);
            }
            if (drift_amplitude != 0.0f)
            {
                input += drift_amplitude *
                    sinf(2.0f * TEST_PI * 1.0f * t);
            }
            if (high_frequency_amplitude != 0.0f)
            {
                input += high_frequency_amplitude *
                    sinf(2.0f * TEST_PI * 1000.0f * t);
            }

            if (input < 0.0f) input = 0.0f;
            if (input > 4095.0f) input = 4095.0f;
            AppFanFeedbackBpf_ProcessSample((uint16_t)lrintf(input));
            (*sample_index)++;
        }
        AppFanFeedbackBpf_EndBlock();
    }
}

static AppFanFeedbackBpfStats run_tach_signal(float frequency_hz,
                                               float amplitude,
                                               float duration_s,
                                               float drift_amplitude,
                                               float high_frequency_amplitude)
{
    uint64_t sample_index = 0ULL;
    AppFanFeedbackBpfStats stats;

    AppFanFeedbackBpf_Init();
    feed_tach_segment(&sample_index, frequency_hz, amplitude, 2048.0f,
                      duration_s, drift_amplitude,
                      high_frequency_amplitude);
    assert(AppFanFeedbackBpf_GetStats(&stats));
    return stats;
}

static void test_shadow_tachometer(void)
{
    AppFanFeedbackBpfStats stats;
    uint64_t sample_index;

    stats = run_tach_signal(33.333333f, 800.0f, 2.0f, 0.0f, 0.0f);
    printf("Tach 33.33 Hz: valid=%u f=%u mHz rpm=%u period=%u hyst=%.3f\n",
           stats.tach_valid, stats.tach_freq_millihz, stats.tach_rpm,
           stats.tach_period_samples, stats.tach_hysteresis);
    assert(stats.tach_shadow_enabled == 1U);
    assert(stats.tach_valid == 1U);
    assert(stats.tach_freq_millihz >= 33250U &&
           stats.tach_freq_millihz <= 33420U);
    assert(stats.tach_rpm >= 998U && stats.tach_rpm <= 1003U);
    assert(stats.tach_period_samples >= 299U &&
           stats.tach_period_samples <= 301U);
    assert(stats.tach_period_history_count == 5U);

    stats = run_tach_signal(76.666667f, 600.0f, 2.0f, 0.0f, 0.0f);
    printf("Tach 76.67 Hz: valid=%u f=%u mHz rpm=%u period=%u\n",
           stats.tach_valid, stats.tach_freq_millihz, stats.tach_rpm,
           stats.tach_period_samples);
    assert(stats.tach_valid == 1U);
    assert(stats.tach_freq_millihz >= 76500U &&
           stats.tach_freq_millihz <= 76850U);
    assert(stats.tach_rpm >= 2295U && stats.tach_rpm <= 2306U);

    stats = run_tach_signal(50.0f, 450.0f, 2.5f, 350.0f, 500.0f);
    printf("Tach 50 Hz + drift + 1 kHz: valid=%u f=%u mHz rpm=%u\n",
           stats.tach_valid, stats.tach_freq_millihz, stats.tach_rpm);
    assert(stats.tach_valid == 1U);
    assert(stats.tach_freq_millihz >= 49850U &&
           stats.tach_freq_millihz <= 50150U);
    assert(stats.tach_rpm >= 1495U && stats.tach_rpm <= 1505U);

    AppFanFeedbackBpf_Init();
    sample_index = 0ULL;
    feed_tach_segment(&sample_index, 33.333333f, 900.0f, 2048.0f,
                      1.2f, 0.0f, 0.0f);
    feed_tach_segment(&sample_index, 33.333333f, 120.0f, 2048.0f,
                      0.8f, 0.0f, 0.0f);
    feed_tach_segment(&sample_index, 33.333333f, 700.0f, 2048.0f,
                      1.0f, 0.0f, 0.0f);
    assert(AppFanFeedbackBpf_GetStats(&stats));
    printf("Tach amplitude changes: valid=%u f=%u mHz rpm=%u rejects=%u\n",
           stats.tach_valid, stats.tach_freq_millihz, stats.tach_rpm,
           stats.tach_inconsistent_rejected);
    assert(stats.tach_valid == 1U);
    assert(stats.tach_freq_millihz >= 33200U &&
           stats.tach_freq_millihz <= 33480U);

    AppFanFeedbackBpf_Init();
    sample_index = 0ULL;
    feed_tach_segment(&sample_index, 33.333333f, 700.0f, 2048.0f,
                      1.4f, 0.0f, 0.0f);
    feed_tach_segment(&sample_index, 60.0f, 700.0f, 2048.0f,
                      1.5f, 0.0f, 0.0f);
    assert(AppFanFeedbackBpf_GetStats(&stats));
    printf("Tach step 33->60 Hz: valid=%u f=%u mHz resync=%u inconsistent=%u\n",
           stats.tach_valid, stats.tach_freq_millihz,
           stats.tach_resync_count, stats.tach_inconsistent_rejected);
    assert(stats.tach_valid == 1U);
    assert(stats.tach_freq_millihz >= 59800U &&
           stats.tach_freq_millihz <= 60200U);
    assert(stats.tach_resync_count >= 1U);

    stats = run_tach_signal(20.0f, 800.0f, 1.5f, 0.0f, 0.0f);
    printf("Tach 20 Hz rejection: valid=%u long=%u\n",
           stats.tach_valid, stats.tach_long_rejected);
    assert(stats.tach_valid == 0U);
    assert(stats.tach_long_rejected > 0U);

    stats = run_tach_signal(100.0f, 800.0f, 1.5f, 0.0f, 0.0f);
    printf("Tach 100 Hz rejection: valid=%u short=%u\n",
           stats.tach_valid, stats.tach_short_rejected);
    assert(stats.tach_valid == 0U);
    assert(stats.tach_short_rejected > 0U);

    AppFanFeedbackBpf_Init();
    sample_index = 0ULL;
    feed_tach_segment(&sample_index, 50.0f, 700.0f, 2048.0f,
                      1.2f, 0.0f, 0.0f);
    assert(AppFanFeedbackBpf_GetStats(&stats));
    assert(stats.tach_valid == 1U);
    feed_tach_segment(&sample_index, 0.0f, 0.0f, 2048.0f,
                      1.0f, 0.0f, 0.0f);
    assert(AppFanFeedbackBpf_GetStats(&stats));
    printf("Tach timeout after signal loss: valid=%u timeout=%u rms=%.3f\n",
           stats.tach_valid, stats.tach_timeout_count, stats.block_rms);
    assert(stats.tach_valid == 0U);
    assert(stats.tach_timeout_count >= 1U);
}

static float gain_for(float frequency_hz)
{
    const float amplitude = 1000.0f;
    float output_rms = run_sine(frequency_hz, amplitude, 2048.0f, 3.0f, 1.0f);
    float input_rms = amplitude / sqrtf(2.0f);
    return output_rms / input_rms;
}

int main(void)
{
    AppFanFeedbackBpfStats stats;

    AppFanFeedbackBpf_Init();
    assert(AppFanFeedbackBpf_GetStats(&stats));
    assert(stats.enabled == 1U);
    assert(stats.warmed_up == 0U);
    assert(stats.warmup_remaining_samples == 1500U);

    AppFanFeedbackBpf_BeginBlock();
    for (uint32_t i = 0U; i < 1500U; i++)
    {
        AppFanFeedbackBpf_ProcessSample(2048U);
    }
    AppFanFeedbackBpf_EndBlock();
    assert(AppFanFeedbackBpf_GetStats(&stats));
    assert(stats.warmed_up == 1U);
    assert(stats.warmup_remaining_samples == 0U);
    assert(stats.nonfinite_count == 0U);

    float dc_rms = run_dc(2048U, 3.0f, 1.0f);
    float gain_10 = gain_for(10.0f);
    float gain_33 = gain_for(33.333333f);
    float gain_50 = gain_for(50.0f);
    float gain_77 = gain_for(76.666667f);
    float gain_150 = gain_for(150.0f);
    float composite_rms = run_composite(3.0f, 1.0f);
    float drift_rms = run_baseline_drift(4.0f, 1.0f);

    printf("DC RMS: %.6f\n", dc_rms);
    printf("Gain 10 Hz: %.6f\n", gain_10);
    printf("Gain 33.33 Hz: %.6f\n", gain_33);
    printf("Gain 50 Hz: %.6f\n", gain_50);
    printf("Gain 76.67 Hz: %.6f\n", gain_77);
    printf("Gain 150 Hz: %.6f\n", gain_150);
    printf("50 Hz + 1 kHz composite RMS: %.6f\n", composite_rms);
    printf("33.33 Hz + 1 Hz baseline drift RMS: %.6f\n", drift_rms);

    assert(dc_rms < 0.05f);
    assert(gain_10 > 0.14f && gain_10 < 0.21f);
    assert(gain_33 > 0.96f && gain_33 < 1.03f);
    assert(gain_50 > 0.97f && gain_50 < 1.03f);
    assert(gain_77 > 0.89f && gain_77 < 0.97f);
    assert(gain_150 > 0.29f && gain_150 < 0.36f);
    assert(composite_rms > 340.0f && composite_rms < 370.0f);
    assert(drift_rms > 540.0f && drift_rms < 590.0f);

    assert(AppFanFeedbackBpf_GetStats(&stats));
    assert(stats.nonfinite_count == 0U);

    test_shadow_tachometer();

    puts("fan BPF and filtered tach host tests passed");
    return 0;
}
