#ifndef TEST_APP_UART_USART_H
#define TEST_APP_UART_USART_H

#include "stm32f4xx_hal.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart6;

#define LED_Pin GPIO_PIN_8
#define LED_GPIO_Port GPIOE

#endif
