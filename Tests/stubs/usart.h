#ifndef TEST_USART_H
#define TEST_USART_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

typedef enum
{
    HAL_OK = 0x00U,
    HAL_ERROR = 0x01U,
    HAL_BUSY = 0x02U
} HAL_StatusTypeDef;

typedef uint32_t HAL_UART_StateTypeDef;

#define HAL_UART_STATE_READY    0x20U
#define HAL_UART_STATE_BUSY_TX  0x21U
#define HAL_UART_STATE_BUSY_RX  0x22U
#define HAL_UART_ERROR_NONE     0x00000000U
#define HAL_UART_ERROR_ORE      0x00000008U

typedef struct
{
    HAL_UART_StateTypeDef gState;
    HAL_UART_StateTypeDef RxState;
    uint32_t ErrorCode;
} UART_HandleTypeDef;

extern UART_HandleTypeDef huart6;

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart,
                                      uint8_t *data,
                                      uint16_t size);
HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data,
                                       uint16_t size);

#endif
