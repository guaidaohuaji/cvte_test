#ifndef TEST_APP_LED_STM32F4XX_HAL_H
#define TEST_APP_LED_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum
{
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET
} GPIO_PinState;

typedef struct
{
    uint32_t ODR;
} GPIO_TypeDef;

extern GPIO_TypeDef test_gpioe;

#define GPIOE (&test_gpioe)
#define GPIO_PIN_8 (1UL << 8U)

uint32_t HAL_GetTick(void);
void HAL_GPIO_WritePin(
    GPIO_TypeDef *port,
    uint16_t pin,
    GPIO_PinState state);

#endif
