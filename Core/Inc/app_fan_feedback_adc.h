#ifndef APP_FAN_FEEDBACK_ADC_H
#define APP_FAN_FEEDBACK_ADC_H

#include <stdbool.h>
#include <stdint.h>

#define APP_FAN_FEEDBACK_DIAG_ENABLED  1U

typedef struct {
    uint8_t  state;
    uint32_t freq_millihz;
    uint32_t period_samples;
    uint32_t update_seq;
    uint16_t raw_min;
    uint16_t raw_max;
    uint32_t rejected_count;
    uint32_t accepted_count;
    uint32_t short_rejected;
    uint32_t long_rejected;
    uint32_t overrun_count;
    uint32_t resync_count;
    uint32_t last_accepted_period;

#if APP_FAN_FEEDBACK_DIAG_ENABLED
    uint16_t raw_average;
    uint16_t raw_span;
    uint16_t filtered_min;
    uint16_t filtered_max;
    uint16_t filtered_average;
    uint16_t filtered_span;
    uint32_t samples_at_or_above_high;
    uint32_t samples_at_or_below_low;
    uint32_t max_consecutive_above_high;
    uint32_t max_consecutive_below_low;
    uint32_t raw_rising_edge_count;
    uint32_t raw_falling_edge_count;
    uint32_t high_candidate_started;
    uint32_t low_candidate_started;
    uint8_t  current_candidate_state;
    uint8_t  current_confirmed_level;
    uint32_t current_candidate_count;
    uint32_t diag_window_time_ms;

    uint32_t high_candidate_rejected_count;
    uint32_t low_candidate_rejected_count;
    uint32_t accepted_period_count;
    uint32_t last_valid_update_age_ms;

    uint16_t current_low_threshold;
    uint16_t current_high_threshold;
    uint16_t current_low_envelope;
    uint16_t current_high_envelope;
    uint16_t current_span;
    uint8_t  envelope_valid;
#endif
} AppFanFeedbackSnapshot;

void AppFanFeedback_Process(void);
bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s);
void AppFanFeedback_ResetMeasurement(void);

#endif
