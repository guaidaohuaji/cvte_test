#ifndef APP_ONEWIRE_UART_H
#define APP_ONEWIRE_UART_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t rx_byte_count;
    uint32_t rx_during_tx_count;
    uint32_t rx_overrun_count;
    uint32_t rx_rearm_fail_count;
    uint32_t uart_error_count;
    uint32_t tx_start_fail_count;
    uint32_t last_error_code;
} AppOneWireUartStats;

void AppOneWireUart_Init(void);
void AppOneWireUart_Process(void);

bool AppOneWireUart_Send(const uint8_t *data, uint8_t length);
bool AppOneWireUart_IsTxBusy(void);
bool AppOneWireUart_TakeTxDone(void);

bool AppOneWireUart_RxAvailable(void);
bool AppOneWireUart_ReadByte(uint8_t *byte);

void AppOneWireUart_RxCpltCallback(void);
void AppOneWireUart_TxCpltCallback(void);
void AppOneWireUart_ErrorCallback(uint32_t error_code);

void AppOneWireUart_GetStats(AppOneWireUartStats *stats);

#endif
