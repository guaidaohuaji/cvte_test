#ifndef TEST_DAMPER_TIM_H
#define TEST_DAMPER_TIM_H

#include <stdint.h>

typedef struct
{
    uint32_t CR1;
    uint32_t DIER;
    uint32_t CNT;
    uint32_t SR;
} TIM_TypeDef;

typedef struct
{
    TIM_TypeDef *Instance;
} TIM_HandleTypeDef;

extern TIM_TypeDef test_tim6_instance;
extern TIM_HandleTypeDef htim6;

#define TIM6 (&test_tim6_instance)
#define TIM_IT_UPDATE   0x0001U
#define TIM_FLAG_UPDATE 0x0001U
#define TIM6_DAC_IRQn   54

#define __HAL_TIM_DISABLE_IT(H, IT) ((H)->Instance->DIER &= ~(uint32_t)(IT))
#define __HAL_TIM_ENABLE_IT(H, IT)  ((H)->Instance->DIER |= (uint32_t)(IT))
#define __HAL_TIM_DISABLE(H)        ((H)->Instance->CR1 &= ~1U)
#define __HAL_TIM_ENABLE(H)         ((H)->Instance->CR1 |= 1U)
#define __HAL_TIM_SET_COUNTER(H, V) ((H)->Instance->CNT = (uint32_t)(V))
#define __HAL_TIM_CLEAR_FLAG(H, F)  ((H)->Instance->SR &= ~(uint32_t)(F))

void HAL_NVIC_ClearPendingIRQ(int irqn);

#endif
