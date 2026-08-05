#ifndef TEST_APP_UART_STM32F4XX_HAL_H
#define TEST_APP_UART_STM32F4XX_HAL_H

#include <stdint.h>

typedef enum
{
    HAL_OK = 0x00U,
    HAL_ERROR = 0x01U,
    HAL_BUSY = 0x02U,
    HAL_TIMEOUT = 0x03U
} HAL_StatusTypeDef;

typedef enum
{
    GPIO_PIN_RESET = 0U,
    GPIO_PIN_SET
} GPIO_PinState;

typedef struct
{
    void *Instance;
} UART_HandleTypeDef;

typedef struct
{
    uint32_t unused;
} TIM_HandleTypeDef;

typedef struct
{
    uint32_t ODR;
} GPIO_TypeDef;

extern GPIO_TypeDef test_gpioe;
extern GPIO_TypeDef test_gpiod;

#define GPIOE (&test_gpioe)
#define GPIOD (&test_gpiod)
#define GPIO_PIN_8  (1UL << 8U)
#define GPIO_PIN_13 (1UL << 13U)
#define USART1 ((void *)0x40011000UL)
#define USART6 ((void *)0x40011400UL)

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit(
    UART_HandleTypeDef *huart,
    uint8_t *data,
    uint16_t length,
    uint32_t timeout);
HAL_StatusTypeDef HAL_UART_Receive_IT(
    UART_HandleTypeDef *huart,
    uint8_t *data,
    uint16_t length);
void HAL_GPIO_WritePin(
    GPIO_TypeDef *port,
    uint16_t pin,
    GPIO_PinState state);

#endif
