#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_onewire_config.h"
#include "app_onewire_uart.h"
#include "usart.h"

UART_HandleTypeDef huart6;

static uint8_t *fake_rx_ptr;
static const uint8_t *fake_tx_ptr;
static uint16_t fake_tx_size;
static HAL_StatusTypeDef fake_next_rx_status;
static HAL_StatusTypeDef fake_next_tx_status;

HAL_StatusTypeDef HAL_UART_Receive_IT(UART_HandleTypeDef *huart,
                                      uint8_t *data,
                                      uint16_t size)
{
    HAL_StatusTypeDef status = fake_next_rx_status;
    fake_next_rx_status = HAL_OK;

    if ((status != HAL_OK) || (size != 1U))
    {
        return (size == 1U) ? status : HAL_ERROR;
    }

    fake_rx_ptr = data;
    huart->RxState = HAL_UART_STATE_BUSY_RX;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef *huart,
                                       const uint8_t *data,
                                       uint16_t size)
{
    HAL_StatusTypeDef status = fake_next_tx_status;
    fake_next_tx_status = HAL_OK;

    if (status != HAL_OK)
    {
        return status;
    }

    fake_tx_ptr = data;
    fake_tx_size = size;
    huart->gState = HAL_UART_STATE_BUSY_TX;
    return HAL_OK;
}

static void fake_reset(void)
{
    (void)memset(&huart6, 0, sizeof(huart6));
    huart6.gState = HAL_UART_STATE_READY;
    huart6.RxState = HAL_UART_STATE_READY;
    huart6.ErrorCode = HAL_UART_ERROR_NONE;

    fake_rx_ptr = NULL;
    fake_tx_ptr = NULL;
    fake_tx_size = 0U;
    fake_next_rx_status = HAL_OK;
    fake_next_tx_status = HAL_OK;
}

static void fake_receive_byte(uint8_t byte)
{
    assert(fake_rx_ptr != NULL);
    *fake_rx_ptr = byte;
    huart6.RxState = HAL_UART_STATE_READY;
    AppOneWireUart_RxCpltCallback();
}

static void fake_complete_tx(void)
{
    huart6.gState = HAL_UART_STATE_READY;
    AppOneWireUart_TxCpltCallback();
}

static void test_init_and_remote_rx(void)
{
    uint8_t byte = 0U;

    fake_reset();
    AppOneWireUart_Init();

    assert(fake_rx_ptr != NULL);
    assert(huart6.RxState == HAL_UART_STATE_BUSY_RX);
    assert(!AppOneWireUart_RxAvailable());

    fake_receive_byte(0xAAU);
    assert(AppOneWireUart_RxAvailable());
    assert(AppOneWireUart_ReadByte(&byte));
    assert(byte == 0xAAU);
    assert(!AppOneWireUart_ReadByte(&byte));
}

static void test_persistent_tx_and_tx_period_rx_drop(void)
{
    uint8_t frame[] = {0xAAU, 0x01U, 0x02U};
    uint8_t byte = 0U;
    AppOneWireUartStats stats;

    fake_reset();
    AppOneWireUart_Init();

    assert(AppOneWireUart_Send(frame, (uint8_t)sizeof(frame)));
    assert(AppOneWireUart_IsTxBusy());
    assert(fake_tx_size == sizeof(frame));
    assert(fake_tx_ptr != frame);
    assert(memcmp(fake_tx_ptr, frame, sizeof(frame)) == 0);

    frame[0] = 0x55U;
    assert(fake_tx_ptr[0] == 0xAAU);

    fake_receive_byte(0xAAU);
    assert(!AppOneWireUart_RxAvailable());

    fake_complete_tx();
    assert(!AppOneWireUart_IsTxBusy());
    assert(AppOneWireUart_TakeTxDone());
    assert(!AppOneWireUart_TakeTxDone());

    fake_receive_byte(0x02U);
    assert(AppOneWireUart_ReadByte(&byte));
    assert(byte == 0x02U);

    AppOneWireUart_GetStats(&stats);
    assert(stats.rx_during_tx_count == 1U);
}

static void test_rx_rearm_recovery(void)
{
    AppOneWireUartStats stats;

    fake_reset();
    AppOneWireUart_Init();

    fake_next_rx_status = HAL_ERROR;
    fake_receive_byte(0x11U);
    assert(huart6.RxState == HAL_UART_STATE_READY);

    AppOneWireUart_GetStats(&stats);
    assert(stats.rx_rearm_fail_count == 1U);

    AppOneWireUart_Process();
    assert(huart6.RxState == HAL_UART_STATE_BUSY_RX);
}

static void test_uart_error_does_not_release_tx(void)
{
    const uint8_t frame[] = {0xAAU, 0x01U};

    fake_reset();
    AppOneWireUart_Init();
    assert(AppOneWireUart_Send(frame, (uint8_t)sizeof(frame)));

    huart6.RxState = HAL_UART_STATE_READY;
    AppOneWireUart_ErrorCallback(HAL_UART_ERROR_ORE);
    AppOneWireUart_Process();

    assert(AppOneWireUart_IsTxBusy());
    assert(huart6.RxState == HAL_UART_STATE_BUSY_RX);

    fake_complete_tx();
    assert(!AppOneWireUart_IsTxBusy());
}

static void test_ring_overrun(void)
{
    uint16_t i;
    uint8_t byte;
    uint16_t read_count = 0U;
    AppOneWireUartStats stats;

    fake_reset();
    AppOneWireUart_Init();

    for (i = 0U; i < APP_ONEWIRE_UART_RX_RING_SIZE; ++i)
    {
        fake_receive_byte((uint8_t)i);
    }

    while (AppOneWireUart_ReadByte(&byte))
    {
        read_count++;
    }

    assert(read_count == (APP_ONEWIRE_UART_RX_RING_SIZE - 1U));
    AppOneWireUart_GetStats(&stats);
    assert(stats.rx_overrun_count == 1U);
}

int main(void)
{
    test_init_and_remote_rx();
    test_persistent_tx_and_tx_period_rx_drop();
    test_rx_rearm_recovery();
    test_uart_error_does_not_release_tx();
    test_ring_overrun();

    puts("app_onewire_uart tests passed");
    return 0;
}
