#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_onewire_config.h"
#include "app_onewire_protocol.h"
#include "app_onewire_slave.h"
#include "app_onewire_uart.h"

#define FAKE_RX_CAPACITY 256U

static uint32_t fake_tick;
static uint8_t fake_rx[FAKE_RX_CAPACITY];
static uint16_t fake_rx_head;
static uint16_t fake_rx_tail;
static uint8_t fake_tx[APP_ONEWIRE_MAX_FRAME_LEN];
static uint8_t fake_tx_length;
static bool fake_tx_busy;
static bool fake_tx_done;
static bool fake_send_fail_once;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

static uint16_t fake_next(uint16_t value)
{
    return (uint16_t)((value + 1U) % FAKE_RX_CAPACITY);
}

bool AppOneWireUart_ReadByte(uint8_t *byte)
{
    if ((byte == NULL) || (fake_rx_head == fake_rx_tail))
    {
        return false;
    }

    *byte = fake_rx[fake_rx_tail];
    fake_rx_tail = fake_next(fake_rx_tail);
    return true;
}

bool AppOneWireUart_Send(const uint8_t *data, uint8_t length)
{
    if (fake_send_fail_once)
    {
        fake_send_fail_once = false;
        return false;
    }

    if (fake_tx_busy || (data == NULL) ||
        (length == 0U) || (length > sizeof(fake_tx)))
    {
        return false;
    }

    memcpy(fake_tx, data, length);
    fake_tx_length = length;
    fake_tx_busy = true;
    fake_tx_done = false;
    return true;
}

bool AppOneWireUart_IsTxBusy(void)
{
    return fake_tx_busy;
}

bool AppOneWireUart_TakeTxDone(void)
{
    bool value = fake_tx_done;
    fake_tx_done = false;
    return value;
}

static void fake_reset(void)
{
    fake_tick = 0U;
    fake_rx_head = 0U;
    fake_rx_tail = 0U;
    fake_tx_length = 0U;
    fake_tx_busy = false;
    fake_tx_done = false;
    fake_send_fail_once = false;
    memset(fake_tx, 0, sizeof(fake_tx));
    AppOneWireSlave_Init();
}

static void fake_push_bytes(const uint8_t *data, uint8_t length)
{
    uint8_t index;

    for (index = 0U; index < length; ++index)
    {
        uint16_t next = fake_next(fake_rx_head);
        assert(next != fake_rx_tail);
        fake_rx[fake_rx_head] = data[index];
        fake_rx_head = next;
    }
}

static void fake_push_frame(const uint8_t *data, uint8_t data_length)
{
    uint8_t frame[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        APP_ONEWIRE_LOCAL_ADDR_SLAVE,
        data,
        data_length,
        frame,
        sizeof(frame));

    assert(length != 0U);
    fake_push_bytes(frame, length);
}

static void fake_complete_tx(void)
{
    assert(fake_tx_busy);
    fake_tx_busy = false;
    fake_tx_done = true;
    AppOneWireSlave_Process();
}

static void assert_tx_equals(const uint8_t *expected, uint8_t length)
{
    assert(fake_tx_length == length);
    assert(memcmp(fake_tx, expected, length) == 0);
}

static void perform_handshake(void)
{
    const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};
    const uint8_t hs2[] = {0x04U, 0x03U, 0x02U, 0x01U};
    const uint8_t hs1_response[] = {
        0xAAU, 0x02U, 0x01U, 0x04U,
        0x04U, 0x03U, 0x02U, 0x01U, 0xA9U
    };
    const uint8_t hs2_response[] = {
        0xAAU, 0x02U, 0x01U, 0x04U,
        0x01U, 0x02U, 0x03U, 0x04U, 0xA9U
    };

    fake_push_frame(hs1, sizeof(hs1));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_IsResponsePending());
    assert(!fake_tx_busy);
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1);

    fake_tick = 1U;
    AppOneWireSlave_Process();
    assert(!fake_tx_busy);

    fake_tick = 2U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);
    assert_tx_equals(hs1_response, sizeof(hs1_response));
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1);

    fake_complete_tx();
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2);

    fake_tick = 10U;
    fake_push_frame(hs2, sizeof(hs2));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_IsResponsePending());

    fake_tick = 12U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);
    assert_tx_equals(hs2_response, sizeof(hs2_response));
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2);

    fake_complete_tx();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_ONLINE);
}

static void test_handshake_and_turnaround(void)
{
    AppOneWireSlaveStats stats;

    fake_reset();
    perform_handshake();

    AppOneWireSlave_GetStats(&stats);
    assert(stats.handshake_1_count == 1U);
    assert(stats.handshake_2_count == 1U);
    assert(stats.response_sent_count == 2U);
}

static void test_write_and_read(void)
{
    const uint8_t write_data[] = {
        APP_ONEWIRE_OPERATION_WRITE, 0x00U, 0x08U, 0x12U, 0x34U
    };
    const uint8_t write_response[] = {
        0xAAU, 0x02U, 0x01U, 0x05U,
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U, 0x81U
    };
    const uint8_t read_data[] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x08U, 0x00U, 0x00U
    };
    const uint8_t read_response[] = {
        0xAAU, 0x02U, 0x01U, 0x05U,
        0x06U, 0x00U, 0x08U, 0x12U, 0x34U, 0x84U
    };
    uint16_t value = 0U;

    fake_reset();
    perform_handshake();

    fake_tick = 20U;
    fake_push_frame(write_data, sizeof(write_data));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_ReadRegister(0x0008U, &value));
    assert(value == 0x1234U);

    fake_tick = 22U;
    AppOneWireSlave_Process();
    assert_tx_equals(write_response, sizeof(write_response));
    fake_complete_tx();

    fake_tick = 30U;
    fake_push_frame(read_data, sizeof(read_data));
    AppOneWireSlave_Process();

    fake_tick = 32U;
    AppOneWireSlave_Process();
    assert_tx_equals(read_response, sizeof(read_response));
    fake_complete_tx();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_ONLINE);
}

static void test_send_failure_keeps_response_pending(void)
{
    const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};
    AppOneWireSlaveStats stats;

    fake_reset();
    fake_push_frame(hs1, sizeof(hs1));
    AppOneWireSlave_Process();

    fake_send_fail_once = true;
    fake_tick = 2U;
    AppOneWireSlave_Process();
    assert(!fake_tx_busy);
    assert(AppOneWireSlave_IsResponsePending());

    fake_tick = 3U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);

    AppOneWireSlave_GetStats(&stats);
    assert(stats.response_send_fail_count == 1U);
}

static void test_fault_and_rehandshake(void)
{
    const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_reset();
    perform_handshake();

    fake_tick = 509U;
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_ONLINE);

    fake_tick = 510U;
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_COMM_FAULT);

    fake_tick = 520U;
    fake_push_frame(hs1, sizeof(hs1));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_IsResponsePending());

    fake_tick = 522U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);
    fake_complete_tx();
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2);
}

static void test_handshake_timeout(void)
{
    const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_reset();
    fake_push_frame(hs1, sizeof(hs1));
    AppOneWireSlave_Process();
    fake_tick = 2U;
    AppOneWireSlave_Process();
    fake_complete_tx();

    fake_tick = 500U;
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1);
}

int main(void)
{
    test_handshake_and_turnaround();
    test_write_and_read();
    test_send_failure_keeps_response_pending();
    test_fault_and_rehandshake();
    test_handshake_timeout();

    puts("app_onewire_slave tests passed");
    return 0;
}
