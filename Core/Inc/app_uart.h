#ifndef APP_UART_H
#define APP_UART_H

#include "stm32f4xx_hal.h"

void AppUart_Init(void);
void AppUart_Process(void);
void AppUart_RxCpltCallback(void);
void AppUart_ErrorCallback(UART_HandleTypeDef *huart);

#endif
