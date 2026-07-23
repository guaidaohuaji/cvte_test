#ifndef APP_FAN_FEEDBACK_ADC_H
#define APP_FAN_FEEDBACK_ADC_H

#include <stdbool.h>
#include <stdint.h>

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
} AppFanFeedbackSnapshot;

void AppFanFeedback_Process(void);
bool AppFanFeedback_GetSnapshot(AppFanFeedbackSnapshot *s);
void AppFanFeedback_ResetMeasurement(void);

#endif
