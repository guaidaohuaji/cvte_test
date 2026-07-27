#ifndef APP_PWM_INPUT_H
#define APP_PWM_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

typedef enum {
    APP_PWM_IN_SEARCH = 0,
    APP_PWM_IN_OK,
    APP_PWM_IN_STATIC_HIGH,
    APP_PWM_IN_STATIC_LOW,
    APP_PWM_IN_OUT_OF_RANGE,
    APP_PWM_IN_UNSTABLE,
    APP_PWM_IN_HW_ERROR
} AppPwmInputStatus;

typedef struct {
    AppPwmInputStatus status;
    uint32_t freq_millihz;
    uint32_t duty_x100;
    uint32_t period_ticks;
    uint32_t ext_high_ticks;
    uint32_t age_ms;
    uint32_t ovc_count;
    uint16_t prescaler;
    uint8_t  synced;
    uint8_t  raw_pin_level;
} AppPwmInputSnapshot;

bool AppPwmInput_Init(void);
void AppPwmInput_Process(void);
void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim);
void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim);
bool AppPwmInput_GetSnapshot(AppPwmInputSnapshot *snap);

#endif
