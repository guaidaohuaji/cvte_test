#ifndef TEST_PWM_TIM_H
#define TEST_PWM_TIM_H

#include <stdint.h>

typedef enum
{
    HAL_OK = 0,
    HAL_ERROR = 1
} HAL_StatusTypeDef;

typedef struct
{
    uint32_t CR1;
    uint32_t CCMR1;
    uint32_t CCER;
    uint32_t CNT;
    uint32_t PSC;
    uint32_t ARR;
    uint32_t CCR1;
    uint32_t EGR;
    uint32_t SR;
} TIM_TypeDef;

typedef struct
{
    TIM_TypeDef *Instance;
} TIM_HandleTypeDef;

#define TIM_CHANNEL_1 1U
#define TIM_CCMR1_OC1M (7UL << 4U)
#define TIM_OCMODE_FORCED_INACTIVE (4UL << 4U)
#define TIM_OCMODE_FORCED_ACTIVE   (5UL << 4U)
#define TIM_OCMODE_PWM1            (6UL << 4U)
#define TIM_EGR_UG 1U
#define TIM_FLAG_UPDATE 1U

#define MODIFY_REG(REG, CLEARMASK, SETMASK) \
    do { (REG) = (((REG) & ~(CLEARMASK)) | (SETMASK)); } while (0)
#define __HAL_TIM_SET_PRESCALER(H, V) ((H)->Instance->PSC = (uint32_t)(V))
#define __HAL_TIM_SET_AUTORELOAD(H, V) ((H)->Instance->ARR = (uint32_t)(V))
#define __HAL_TIM_SET_COMPARE(H, C, V) \
    do { (void)(C); (H)->Instance->CCR1 = (uint32_t)(V); } while (0)
#define __HAL_TIM_SET_COUNTER(H, V) ((H)->Instance->CNT = (uint32_t)(V))
#define __HAL_TIM_CLEAR_FLAG(H, F) \
    do { (H)->Instance->SR &= ~(uint32_t)(F); } while (0)

extern TIM_HandleTypeDef htim4;
HAL_StatusTypeDef HAL_TIM_PWM_Start(TIM_HandleTypeDef *handle, uint32_t channel);
HAL_StatusTypeDef HAL_TIM_PWM_Stop(TIM_HandleTypeDef *handle, uint32_t channel);

#endif
