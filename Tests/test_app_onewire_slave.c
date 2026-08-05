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
    if (fake_tx_busy || (data == NULL) || (length == 0U))
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
    bool result = fake_tx_done;
    fake_tx_done = false;
    return result;
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

static void fake_push_frame_to(
    uint8_t destination,
    const uint8_t *data,
    uint8_t data_length)
{
    uint8_t frame[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t frame_length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        destination,
        data,
        data_length,
        frame,
        (uint8_t)sizeof(frame));
    uint8_t index;

    assert(frame_length != 0U);
    for (index = 0U; index < frame_length; ++index)
    {
        uint16_t next = fake_next(fake_rx_head);
        assert(next != fake_rx_tail);
        fake_rx[fake_rx_head] = frame[index];
        fake_rx_head = next;
    }
}

static void assert_tx_frame(const uint8_t *data, uint8_t data_length)
{
    uint8_t expected[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t expected_length = AppOneWire_BuildFrame(
        APP_ONEWIRE_SLAVE_ADDRESS,
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        data,
        data_length,
        expected,
        (uint8_t)sizeof(expected));

    assert(fake_tx_length == expected_length);
    assert(memcmp(fake_tx, expected, expected_length) == 0);
}

static void fake_complete_tx(void)
{
    assert(fake_tx_busy);
    fake_tx_busy = false;
    fake_tx_done = true;
    AppOneWireSlave_Process();
}

static void perform_handshake(void)
{
    static const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};
    static const uint8_t hs1_response[] = {0x04U, 0x03U, 0x02U, 0x01U};
    static const uint8_t hs2[] = {0x04U, 0x03U, 0x02U, 0x01U};
    static const uint8_t hs2_response[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_push_frame_to(APP_ONEWIRE_SLAVE_ADDRESS, hs1, sizeof(hs1));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_IsResponsePending());

    fake_tick = 2U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);
    assert_tx_frame(hs1_response, sizeof(hs1_response));
    fake_complete_tx();
    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2);

    fake_tick = 10U;
    fake_push_frame_to(APP_ONEWIRE_SLAVE_ADDRESS, hs2, sizeof(hs2));
    AppOneWireSlave_Process();
    fake_tick = 12U;
    AppOneWireSlave_Process();
    assert(fake_tx_busy);
    assert_tx_frame(hs2_response, sizeof(hs2_response));
    fake_complete_tx();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_ONLINE);
}

static void test_addressed_handshake_and_io(void)
{
    const uint8_t write_data[] = {
        APP_ONEWIRE_OPERATION_WRITE, 0x00U, 0x08U, 0x12U, 0x34U
    };
    const uint8_t read_data[] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x08U, 0x00U, 0x00U
    };
    const uint8_t read_response[] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x08U, 0x12U, 0x34U
    };
    uint16_t value;

    fake_reset();
    perform_handshake();

    fake_tick = 20U;
    fake_push_frame_to(APP_ONEWIRE_SLAVE_ADDRESS,
                       write_data,
                       sizeof(write_data));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_ReadRegister(0x0008U, &value));
    assert(value == 0x1234U);
    fake_tick = 22U;
    AppOneWireSlave_Process();
    assert_tx_frame(write_data, sizeof(write_data));
    fake_complete_tx();

    fake_tick = 30U;
    fake_push_frame_to(APP_ONEWIRE_SLAVE_ADDRESS,
                       read_data,
                       sizeof(read_data));
    AppOneWireSlave_Process();
    fake_tick = 32U;
    AppOneWireSlave_Process();
    assert_tx_frame(read_response, sizeof(read_response));
    fake_complete_tx();
}

static void test_foreign_frames_are_silent_and_do_not_refresh_timeout(void)
{
    static const uint8_t read_data[] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x08U, 0x00U, 0x00U
    };
    AppOneWireSlaveStats stats;
    uint8_t other_address =
        (APP_ONEWIRE_SLAVE_ADDRESS == 0x03U) ? 0x04U : 0x03U;

    fake_reset();
    perform_handshake();

    fake_tick = 400U;
    fake_push_frame_to(other_address, read_data, sizeof(read_data));
    AppOneWireSlave_Process();
    assert(!fake_tx_busy);
    assert(!AppOneWireSlave_IsResponsePending());
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_ONLINE);

    /* Local handshake completed at tick 10. Foreign traffic at 400 must not
     * refresh the 500 ms local watchdog. */
    fake_tick = 510U;
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_COMM_FAULT);

    AppOneWireSlave_GetStats(&stats);
    assert(stats.ignored_foreign_frame_count == 1U);
}

static void test_rehandshake_after_fault(void)
{
    static const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_reset();
    perform_handshake();
    fake_tick = 510U;
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_GetState() == APP_ONEWIRE_SLAVE_COMM_FAULT);

    fake_tick = 520U;
    fake_push_frame_to(APP_ONEWIRE_SLAVE_ADDRESS, hs1, sizeof(hs1));
    AppOneWireSlave_Process();
    assert(AppOneWireSlave_IsResponsePending());
}

static void test_wrong_address_never_starts_handshake(void)
{
    static const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};
    uint8_t other_address =
        (APP_ONEWIRE_SLAVE_ADDRESS == 0x03U) ? 0x04U : 0x03U;

    fake_reset();
    fake_push_frame_to(other_address, hs1, sizeof(hs1));
    AppOneWireSlave_Process();

    assert(AppOneWireSlave_GetState() ==
           APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1);
    assert(!AppOneWireSlave_IsResponsePending());
    assert(!fake_tx_busy);
}

int main(void)
{
    test_addressed_handshake_and_io();
    test_foreign_frames_are_silent_and_do_not_refresh_timeout();
    test_rehandshake_after_fault();
    test_wrong_address_never_starts_handshake();

    puts("app_onewire_slave addressed multi-drop tests passed");
    return 0;
}
