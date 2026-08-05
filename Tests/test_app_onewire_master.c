#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_onewire_config.h"
#include "app_onewire_master.h"
#include "app_onewire_protocol.h"
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
static AppOneWireUartStats fake_uart_stats;

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

void AppOneWireUart_GetStats(AppOneWireUartStats *stats)
{
    assert(stats != NULL);
    *stats = fake_uart_stats;
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
    memset(&fake_uart_stats, 0, sizeof(fake_uart_stats));
    AppOneWireMaster_Init();
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

static void fake_push_slave_frame(
    uint8_t slave_address,
    const uint8_t *data,
    uint8_t data_length)
{
    uint8_t frame[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t length = AppOneWire_BuildFrame(
        slave_address,
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        data,
        data_length,
        frame,
        (uint8_t)sizeof(frame));

    assert(length != 0U);
    fake_push_bytes(frame, length);
}

static void fake_complete_tx(uint32_t tick)
{
    assert(fake_tx_busy);
    fake_tick = tick;
    fake_tx_busy = false;
    fake_tx_done = true;
    AppOneWireMaster_Process();
}

static void assert_tx_frame(
    uint8_t destination,
    const uint8_t *data,
    uint8_t data_length)
{
    uint8_t expected[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t expected_length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        destination,
        data,
        data_length,
        expected,
        (uint8_t)sizeof(expected));

    assert(expected_length == fake_tx_length);
    assert(memcmp(expected, fake_tx, expected_length) == 0);
}

static uint32_t perform_handshake_until_operation(
    uint8_t slave_address,
    uint32_t start_tick,
    bool expect_operation)
{
    static const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};
    static const uint8_t hs1_response[] = {0x04U, 0x03U, 0x02U, 0x01U};
    static const uint8_t hs2[] = {0x04U, 0x03U, 0x02U, 0x01U};
    static const uint8_t hs2_response[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_tick = start_tick;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_frame(slave_address, hs1, (uint8_t)sizeof(hs1));

    fake_complete_tx(start_tick + 1U);

    fake_tick = start_tick + 3U;
    fake_push_slave_frame(slave_address,
                          hs1_response,
                          (uint8_t)sizeof(hs1_response));
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2);

    fake_tick = start_tick + 6U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_frame(slave_address, hs2, (uint8_t)sizeof(hs2));

    fake_complete_tx(start_tick + 7U);

    fake_tick = start_tick + 9U;
    fake_push_slave_frame(slave_address,
                          hs2_response,
                          (uint8_t)sizeof(hs2_response));
    AppOneWireMaster_Process();

    if (expect_operation)
    {
        assert(AppOneWireMaster_GetState() ==
               APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION);
    }
    else
    {
        assert(AppOneWireMaster_GetState() == APP_ONEWIRE_MASTER_IDLE);
        assert(!AppOneWireMaster_IsBusy());
    }

    return start_tick + 9U;
}

static uint32_t finish_write_operation(
    uint8_t slave_address,
    uint16_t address,
    uint16_t value,
    uint32_t handshake_done_tick)
{
    uint8_t operation[5] = {
        APP_ONEWIRE_OPERATION_WRITE,
        (uint8_t)(address >> 8U),
        (uint8_t)address,
        (uint8_t)(value >> 8U),
        (uint8_t)value
    };

    fake_tick = handshake_done_tick + 3U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_frame(slave_address, operation, (uint8_t)sizeof(operation));

    fake_complete_tx(handshake_done_tick + 4U);
    fake_tick = handshake_done_tick + 6U;
    fake_push_slave_frame(slave_address,
                          operation,
                          (uint8_t)sizeof(operation));
    AppOneWireMaster_Process();
    assert(!AppOneWireMaster_IsBusy());
    return handshake_done_tick + 6U;
}

static void test_no_boot_handshake(void)
{
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    AppOneWireMaster_Process();
    fake_tick = 1000U;
    AppOneWireMaster_Process();

    assert(!fake_tx_busy);
    assert(AppOneWireMaster_GetState() == APP_ONEWIRE_MASTER_IDLE);
    assert(!AppOneWireMaster_IsBusy());
    assert(AppOneWireMaster_GetSnapshotForAddress(0x02U, &snapshot));
    assert(snapshot.context_valid);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_OFFLINE);
    assert(snapshot.last_response_age_ms == 0xFFFFU);
}

static void test_targeted_handshake_then_write(void)
{
    AppOneWireMasterSnapshot snapshot;
    uint32_t handshake_done;
    uint32_t done_tick;

    fake_reset();
    assert(AppOneWireMaster_SubmitTo(
               0x03U,
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);

    handshake_done = perform_handshake_until_operation(0x03U, 0U, true);
    done_tick = finish_write_operation(
        0x03U, 0x0008U, 0x1234U, handshake_done);

    fake_tick = done_tick;
    assert(AppOneWireMaster_GetSnapshotForAddress(0x03U, &snapshot));
    assert(snapshot.link_state == APP_ONEWIRE_LINK_ONLINE);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
    assert(snapshot.address == 0x0008U);
    assert(snapshot.value == 0x1234U);
    assert(snapshot.last_response_age_ms == 0U);

    assert(AppOneWireMaster_GetSnapshotForAddress(0x02U, &snapshot));
    assert(snapshot.link_state == APP_ONEWIRE_LINK_OFFLINE);
}

static void test_fresh_link_skips_handshake(void)
{
    uint8_t read_data[5] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x09U, 0x00U, 0x00U
    };
    uint8_t read_response[5] = {
        APP_ONEWIRE_OPERATION_READ, 0x00U, 0x09U, 0x56U, 0x78U
    };
    AppOneWireMasterSnapshot snapshot;
    uint32_t handshake_done;
    uint32_t done_tick;

    fake_reset();
    assert(AppOneWireMaster_SubmitTo(
               0x03U,
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);
    handshake_done = perform_handshake_until_operation(0x03U, 0U, true);
    done_tick = finish_write_operation(
        0x03U, 0x0008U, 0x1234U, handshake_done);

    fake_tick = done_tick + 1U;
    assert(AppOneWireMaster_SubmitTo(
               0x03U,
               APP_ONEWIRE_OPERATION_READ,
               0x0009U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    assert(!fake_tx_busy); /* 3 ms guard after last response. */

    fake_tick = done_tick + 3U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_frame(0x03U, read_data, (uint8_t)sizeof(read_data));

    fake_complete_tx(done_tick + 4U);
    fake_tick = done_tick + 6U;
    fake_push_slave_frame(0x03U,
                          read_response,
                          (uint8_t)sizeof(read_response));
    AppOneWireMaster_Process();

    assert(AppOneWireMaster_GetSnapshotForAddress(0x03U, &snapshot));
    assert(snapshot.value == 0x5678U);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
}

static void test_stale_link_rehandshakes_on_next_command(void)
{
    AppOneWireMasterSnapshot snapshot;
    uint32_t handshake_done;
    uint32_t done_tick;
    static const uint8_t hs1[] = {0x01U, 0x02U, 0x03U, 0x04U};

    fake_reset();
    assert(AppOneWireMaster_SubmitTo(
               0x02U,
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);
    handshake_done = perform_handshake_until_operation(0x02U, 0U, true);
    done_tick = finish_write_operation(
        0x02U, 0x0008U, 0x1234U, handshake_done);

    fake_tick = done_tick + APP_ONEWIRE_MASTER_LINK_VALID_MS;
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetSnapshotForAddress(0x02U, &snapshot));
    assert(snapshot.link_state == APP_ONEWIRE_LINK_STALE);

    assert(AppOneWireMaster_SubmitTo(
               0x02U,
               APP_ONEWIRE_OPERATION_READ,
               0x0009U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_frame(0x02U, hs1, (uint8_t)sizeof(hs1));
}

static void test_other_slave_is_ignored_while_waiting(void)
{
    static const uint8_t wrong_hs1_response[] = {
        0x04U, 0x03U, 0x02U, 0x01U
    };
    AppOneWireMasterStats stats;

    fake_reset();
    assert(AppOneWireMaster_SubmitTo(
               0x03U,
               APP_ONEWIRE_OPERATION_REHANDSHAKE,
               0U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    fake_tick = 0U;
    AppOneWireMaster_Process();
    fake_complete_tx(1U);

    fake_tick = 3U;
    fake_push_slave_frame(0x02U,
                          wrong_hs1_response,
                          (uint8_t)sizeof(wrong_hs1_response));
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT);

    AppOneWireMaster_GetStats(&stats);
    assert(stats.unexpected_frame_count == 1U);
}

static void test_busy_and_context_limit(void)
{
    uint8_t address;

    fake_reset();
    assert(AppOneWireMaster_SubmitTo(
               0x02U,
               APP_ONEWIRE_OPERATION_READ,
               0x0008U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    assert(AppOneWireMaster_SubmitTo(
               0x03U,
               APP_ONEWIRE_OPERATION_READ,
               0x0008U,
               0U) == APP_ONEWIRE_SUBMIT_BUSY);

    fake_reset();
    /* 0x02 is preallocated, then allocate seven more contexts. */
    for (address = 0x03U; address <= 0x09U; ++address)
    {
        assert(AppOneWireMaster_SubmitTo(
                   address,
                   APP_ONEWIRE_OPERATION_REHANDSHAKE,
                   0U,
                   0U) == APP_ONEWIRE_SUBMIT_OK);
        /* Force handshake timeout to release the single transaction slot. */
        fake_tick += 1U;
        AppOneWireMaster_Process();
        fake_complete_tx(fake_tick + 1U);
        fake_tick += APP_ONEWIRE_HANDSHAKE_RESPONSE_MS + 2U;
        AppOneWireMaster_Process();
        assert(!AppOneWireMaster_IsBusy());
    }

    assert(AppOneWireMaster_SubmitTo(
               0x0AU,
               APP_ONEWIRE_OPERATION_REHANDSHAKE,
               0U,
               0U) == APP_ONEWIRE_SUBMIT_NO_CONTEXT);
    assert(AppOneWireMaster_SubmitTo(
               0x01U,
               APP_ONEWIRE_OPERATION_READ,
               0x0008U,
               0U) == APP_ONEWIRE_SUBMIT_INVALID_SLAVE);
}

int main(void)
{
    test_no_boot_handshake();
    test_targeted_handshake_then_write();
    test_fresh_link_skips_handshake();
    test_stale_link_rehandshakes_on_next_command();
    test_other_slave_is_ignored_while_waiting();
    test_busy_and_context_limit();

    puts("app_onewire_master multi-slave on-demand tests passed");
    return 0;
}
