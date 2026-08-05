#ifndef TEST_FAN_TIM_H
#define TEST_FAN_TIM_H

#include <stdint.h>

typedef struct {
    uint32_t CCR1;
} TIM_TypeDef;

typedef struct {
    TIM_TypeDef *Instance;
} TIM_HandleTypeDef;

typedef enum {
    HAL_OK = 0,
    HAL_ERROR = 1
} HAL_StatusTypeDef;

#define TIM_CHANNEL_1 1U
#define __HAL_TIM_SET_COMPARE(handle, channel, compare) \
    do { (void)(channel); (handle)->Instance->CCR1 = (uint32_t)(compare); } while (0)

extern TIM_HandleTypeDef htim10;
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *handle, uint32_t channel);
uint32_t HAL_GetTick(void);

#endif
