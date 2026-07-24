#include "app_onewire_uart.h"

#include <stddef.h>

#include "app_onewire_config.h"
#include "usart.h"

#define APP_ONEWIRE_TAIL_ECHO_WINDOW_MS  1U

#if ((APP_ONEWIRE_UART_RX_RING_SIZE == 0U) || \
     ((APP_ONEWIRE_UART_RX_RING_SIZE & (APP_ONEWIRE_UART_RX_RING_SIZE - 1U)) != 0U))
#error "APP_ONEWIRE_UART_RX_RING_SIZE must be a non-zero power of two"
#endif

#if (APP_ONEWIRE_UART_RX_RING_SIZE > 65536U)
#error "APP_ONEWIRE_UART_RX_RING_SIZE exceeds uint16_t index capacity"
#endif

static volatile uint8_t rx_byte;
static volatile uint8_t rx_ring[APP_ONEWIRE_UART_RX_RING_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile uint8_t rx_rearm_pending;

static uint8_t tx_storage[APP_ONEWIRE_MAX_FRAME_LEN];
static volatile uint8_t tx_active;
static volatile uint8_t tx_done;

static volatile AppOneWireUartStats uart_stats;

static volatile uint8_t tail_echo_byte;
static volatile uint8_t tail_echo_active;
static volatile uint32_t tail_echo_tick;

static uint16_t ring_next(uint16_t index)
{
    return (uint16_t)((index + 1U) &
                      (APP_ONEWIRE_UART_RX_RING_SIZE - 1U));
}

static void ring_put(uint8_t byte)
{
    uint16_t head = rx_head;
    uint16_t next = ring_next(head);

    if (next == rx_tail)
    {
        uart_stats.rx_overrun_count++;
        return;
    }

    rx_ring[head] = byte;
    rx_head = next;
}

static bool try_arm_rx(void)
{
    HAL_StatusTypeDef status;

    if (huart6.RxState == HAL_UART_STATE_BUSY_RX)
    {
        rx_rearm_pending = 0U;
        return true;
    }

    if (huart6.RxState != HAL_UART_STATE_READY)
    {
        rx_rearm_pending = 1U;
        return false;
    }

    status = HAL_UART_Receive_IT(&huart6, (uint8_t *)&rx_byte, 1U);
    if (status == HAL_OK)
    {
        rx_rearm_pending = 0U;
        return true;
    }

    rx_rearm_pending = 1U;
    if (status != HAL_BUSY)
    {
        uart_stats.rx_rearm_fail_count++;
    }
    return false;
}

void AppOneWireUart_Init(void)
{
    rx_byte = 0U;
    rx_head = 0U;
    rx_tail = 0U;
    rx_rearm_pending = 0U;

    tx_active = 0U;
    tx_done = 0U;

    tail_echo_byte   = 0U;
    tail_echo_tick   = 0U;
    tail_echo_active = 0U;

    uart_stats.rx_byte_count = 0U;
    uart_stats.rx_during_tx_count = 0U;
    uart_stats.rx_overrun_count = 0U;
    uart_stats.rx_rearm_fail_count = 0U;
    uart_stats.uart_error_count = 0U;
    uart_stats.tx_start_fail_count = 0U;
    uart_stats.last_error_code = HAL_UART_ERROR_NONE;
    uart_stats.tail_echo_dropped = 0U;

    if (!try_arm_rx())
    {
        rx_rearm_pending = 1U;
    }
}

void AppOneWireUart_Process(void)
{
    if (rx_rearm_pending != 0U)
    {
        (void)try_arm_rx();
    }

    if (tail_echo_active != 0U)
    {
        uint32_t now = HAL_GetTick();
        if ((uint32_t)(now - tail_echo_tick) >
            APP_ONEWIRE_TAIL_ECHO_WINDOW_MS)
        {
            tail_echo_active = 0U;
        }
    }
}

bool AppOneWireUart_Send(const uint8_t *data, uint8_t length)
{
    HAL_StatusTypeDef status;

    if ((data == NULL) ||
        (length == 0U) ||
        (length > APP_ONEWIRE_MAX_FRAME_LEN))
    {
        uart_stats.tx_start_fail_count++;
        return false;
    }

    if ((tx_active != 0U) || (huart6.gState != HAL_UART_STATE_READY))
    {
        return false;
    }

    {
        uint8_t index;
        for (index = 0U; index < length; ++index)
        {
            tx_storage[index] = data[index];
        }
    }
    tx_done = 0U;

    tail_echo_byte   = tx_storage[length - 1U];
    tail_echo_active = 0U;

    tx_active = 1U;

    status = HAL_UART_Transmit_IT(&huart6, tx_storage, length);
    if (status != HAL_OK)
    {
        tx_active = 0U;
        tail_echo_active = 0U;
        uart_stats.tx_start_fail_count++;
        return false;
    }

    return true;
}

bool AppOneWireUart_IsTxBusy(void)
{
    return (tx_active != 0U) ||
           (huart6.gState != HAL_UART_STATE_READY);
}

bool AppOneWireUart_TakeTxDone(void)
{
    if (tx_done == 0U)
    {
        return false;
    }

    tx_done = 0U;
    return true;
}

bool AppOneWireUart_RxAvailable(void)
{
    return rx_head != rx_tail;
}

bool AppOneWireUart_ReadByte(uint8_t *byte)
{
    uint16_t tail;

    if ((byte == NULL) || (rx_head == rx_tail))
    {
        return false;
    }

    tail = rx_tail;
    *byte = rx_ring[tail];
    rx_tail = ring_next(tail);
    return true;
}

void AppOneWireUart_RxCpltCallback(void)
{
    uint8_t byte = rx_byte;
    uint32_t now_tick = HAL_GetTick();

    uart_stats.rx_byte_count++;

    if (tx_active != 0U)
    {
        uart_stats.rx_during_tx_count++;
    }
    else if (tail_echo_active != 0U)
    {
        uint32_t elapsed = (uint32_t)(now_tick - tail_echo_tick);

        if ((elapsed <= APP_ONEWIRE_TAIL_ECHO_WINDOW_MS) &&
            (byte == tail_echo_byte))
        {
            tail_echo_active = 0U;
            uart_stats.tail_echo_dropped++;
        }
        else
        {
            tail_echo_active = 0U;
            ring_put(byte);
        }
    }
    else
    {
        ring_put(byte);
    }

    if (!try_arm_rx())
    {
        rx_rearm_pending = 1U;
    }
}

void AppOneWireUart_TxCpltCallback(void)
{
    tail_echo_tick   = HAL_GetTick();
    tail_echo_active = 1U;

    tx_active = 0U;
    tx_done   = 1U;
}

void AppOneWireUart_ErrorCallback(uint32_t error_code)
{
    uart_stats.last_error_code = error_code;
    uart_stats.uart_error_count++;
    rx_rearm_pending = 1U;
    tail_echo_active = 0U;
}

void AppOneWireUart_GetStats(AppOneWireUartStats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    stats->rx_byte_count = uart_stats.rx_byte_count;
    stats->rx_during_tx_count = uart_stats.rx_during_tx_count;
    stats->rx_overrun_count = uart_stats.rx_overrun_count;
    stats->rx_rearm_fail_count = uart_stats.rx_rearm_fail_count;
    stats->uart_error_count = uart_stats.uart_error_count;
    stats->tx_start_fail_count = uart_stats.tx_start_fail_count;
    stats->last_error_code = uart_stats.last_error_code;
    stats->tail_echo_dropped = uart_stats.tail_echo_dropped;
}
