#ifndef TEST_DAMPER_MAIN_H
#define TEST_DAMPER_MAIN_H

#include <stdint.h>

typedef enum
{
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET = 1
} GPIO_PinState;

typedef struct
{
    uint32_t BSRR;
} GPIO_TypeDef;

typedef struct
{
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} GPIO_InitTypeDef;

extern GPIO_TypeDef test_gpio_b;
extern GPIO_TypeDef test_gpio_c;
#define GPIOB (&test_gpio_b)
#define GPIOC (&test_gpio_c)

#define GPIO_PIN_3   (1U << 3)
#define GPIO_PIN_4   (1U << 4)
#define GPIO_PIN_5   (1U << 5)
#define GPIO_PIN_14  (1U << 14)
#define GPIO_PIN_15  (1U << 15)

#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_NOPULL         0U
#define GPIO_SPEED_FREQ_LOW 0U

#define __HAL_RCC_GPIOB_CLK_ENABLE() do { } while (0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() do { } while (0)

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint32_t pins, GPIO_PinState state);
void HAL_GPIO_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init_cfg);
uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __enable_irq(void);

#endif
