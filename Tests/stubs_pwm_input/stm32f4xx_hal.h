#ifndef TEST_PWM_INPUT_STM32F4XX_HAL_H
#define TEST_PWM_INPUT_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

typedef struct { uint32_t IDR; } GPIO_TypeDef;
extern GPIO_TypeDef test_gpioe;
#define GPIOE (&test_gpioe)
#define GPIO_PIN_9 (1U << 9)

typedef struct {
    uint32_t CR1;
    uint32_t DIER;
    uint32_t SR;
    uint32_t EGR;
    uint32_t CNT;
    uint32_t PSC;
    uint32_t ARR;
    uint32_t CCR1;
} TIM_TypeDef;

extern TIM_TypeDef test_tim1;
#define TIM1 (&test_tim1)

typedef struct { TIM_TypeDef *Instance; } TIM_HandleTypeDef;

#define TIM_CHANNEL_1 1U
#define TIM_IT_UPDATE (1U << 0)
#define TIM_FLAG_UPDATE (1U << 0)
#define TIM_FLAG_CC1 (1U << 1)
#define TIM_FLAG_CC1OF (1U << 9)
#define TIM_SR_UIF TIM_FLAG_UPDATE
#define TIM_SR_CC1OF TIM_FLAG_CC1OF
#define TIM_EGR_UG 1U

#define __HAL_TIM_DISABLE(H) do { (H)->Instance->CR1 &= ~1U; } while (0)
#define __HAL_TIM_DISABLE_IT(H, I) do { (H)->Instance->DIER &= ~(I); } while (0)
#define __HAL_TIM_ENABLE_IT(H, I) do { (H)->Instance->DIER |= (I); } while (0)
#define __HAL_TIM_SET_COUNTER(H, V) do { (H)->Instance->CNT = (V); } while (0)
#define __HAL_TIM_SET_PRESCALER(H, V) do { (H)->Instance->PSC = (V); } while (0)
#define __HAL_TIM_CLEAR_FLAG(H, F) do { (H)->Instance->SR &= ~(F); } while (0)

uint32_t HAL_GetTick(void);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
HAL_StatusTypeDef HAL_TIM_IC_Start_IT(TIM_HandleTypeDef *htim, uint32_t channel);

#endif
