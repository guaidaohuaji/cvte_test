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

static void fake_push_slave_frame(const uint8_t *data, uint8_t data_length)
{
    uint8_t frame[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_SLAVE,
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

static void assert_tx_equals(const uint8_t *expected, uint8_t length)
{
    assert(fake_tx_length == length);
    assert(memcmp(fake_tx, expected, length) == 0);
}

static void start_boot_handshake(void)
{
    static const uint8_t expected_hs1[] = {
        0xAAU, 0x01U, 0x02U, 0x04U,
        0x01U, 0x02U, 0x03U, 0x04U, 0xA9U
    };

    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_hs1, (uint8_t)sizeof(expected_hs1));
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_HANDSHAKE_1_TX);
}

static void perform_boot_handshake(void)
{
    static const uint8_t hs1_response_data[] = {
        0x04U, 0x03U, 0x02U, 0x01U
    };
    static const uint8_t hs2_response_data[] = {
        0x01U, 0x02U, 0x03U, 0x04U
    };
    static const uint8_t expected_hs2[] = {
        0xAAU, 0x01U, 0x02U, 0x04U,
        0x04U, 0x03U, 0x02U, 0x01U, 0xA9U
    };

    start_boot_handshake();
    fake_complete_tx(1U);
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT);

    fake_tick = 3U;
    fake_push_slave_frame(hs1_response_data,
                          (uint8_t)sizeof(hs1_response_data));
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2);

    fake_tick = 5U;
    AppOneWireMaster_Process();
    assert(!fake_tx_busy);

    fake_tick = 6U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_hs2, (uint8_t)sizeof(expected_hs2));

    fake_complete_tx(7U);
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT);

    fake_tick = 9U;
    fake_push_slave_frame(hs2_response_data,
                          (uint8_t)sizeof(hs2_response_data));
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_ONLINE_IDLE);
    assert(AppOneWireMaster_GetLinkState() == APP_ONEWIRE_LINK_ONLINE);
    assert(!AppOneWireMaster_IsBusy());
}


static void test_tx_done_and_response_same_process(void)
{
    static const uint8_t hs1_response_data[] = {
        0x04U, 0x03U, 0x02U, 0x01U
    };

    fake_reset();
    start_boot_handshake();

    fake_tick = 3U;
    fake_tx_busy = false;
    fake_tx_done = true;
    fake_push_slave_frame(hs1_response_data,
                          (uint8_t)sizeof(hs1_response_data));
    AppOneWireMaster_Process();

    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2);
}

static void test_explicit_rehandshake_honors_guard(void)
{
    static const uint8_t expected_hs1[] = {
        0xAAU, 0x01U, 0x02U, 0x04U,
        0x01U, 0x02U, 0x03U, 0x04U, 0xA9U
    };
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    perform_boot_handshake();

    fake_tick = 10U;
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_REHANDSHAKE,
               0x1234U,
               0x5678U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_PENDING);
    assert(snapshot.busy);
    assert(snapshot.address == 0U);
    assert(snapshot.value == 0U);

    AppOneWireMaster_Process();
    assert(!fake_tx_busy);

    fake_tick = 12U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_hs1, (uint8_t)sizeof(expected_hs1));
}

static void test_boot_handshake_and_guard(void)
{
    AppOneWireMasterSnapshot snapshot;
    AppOneWireMasterStats stats;

    fake_reset();
    perform_boot_handshake();

    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
    assert(snapshot.last_operation == APP_ONEWIRE_OPERATION_REHANDSHAKE);

    AppOneWireMaster_GetStats(&stats);
    assert(stats.handshake_attempt_count == 1U);
    assert(stats.handshake_1_ok_count == 1U);
    assert(stats.handshake_2_ok_count == 1U);
}

static void test_handshake_response_timeout(void)
{
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    start_boot_handshake();
    fake_complete_tx(1U);

    fake_tick = 100U;
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_IsBusy());

    fake_tick = 101U;
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(!snapshot.busy);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_STALE);
    assert(snapshot.result_code ==
           APP_ONEWIRE_RESULT_HANDSHAKE_FAILED);
}

static void test_write_after_handshake_guard(void)
{
    static const uint8_t expected_write[] = {
        0xAAU, 0x01U, 0x02U, 0x05U,
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U, 0x81U
    };
    static const uint8_t write_response_data[] = {
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U
    };
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    perform_boot_handshake();

    fake_tick = 10U;
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    assert(!fake_tx_busy);

    fake_tick = 12U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_write, (uint8_t)sizeof(expected_write));

    fake_complete_tx(13U);
    fake_tick = 15U;
    fake_push_slave_frame(write_response_data,
                          (uint8_t)sizeof(write_response_data));
    AppOneWireMaster_Process();

    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(!snapshot.busy);
    assert(!snapshot.pending_valid);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
    assert(snapshot.address == 0x0008U);
    assert(snapshot.value == 0x1234U);
}

static void test_read_response_updates_value(void)
{
    static const uint8_t expected_read[] = {
        0xAAU, 0x01U, 0x02U, 0x05U,
        0x06U, 0x00U, 0x08U, 0x00U, 0x00U, 0xA2U
    };
    static const uint8_t read_response_data[] = {
        0x06U, 0x00U, 0x08U, 0x12U, 0x34U
    };
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    perform_boot_handshake();

    fake_tick = 12U;
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_READ,
               0x0008U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_read, (uint8_t)sizeof(expected_read));

    fake_complete_tx(13U);
    fake_tick = 14U;
    fake_push_slave_frame(read_response_data,
                          (uint8_t)sizeof(read_response_data));
    AppOneWireMaster_Process();

    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
    assert(snapshot.last_operation == APP_ONEWIRE_OPERATION_READ);
    assert(snapshot.address == 0x0008U);
    assert(snapshot.value == 0x1234U);
}

static void test_stale_link_preserves_last_result(void)
{
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    perform_boot_handshake();

    fake_tick = 459U;
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_STALE);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_SUCCESS);
}

static void test_stale_write_rehandshakes_then_sends_once(void)
{
    static const uint8_t hs1_response_data[] = {
        0x04U, 0x03U, 0x02U, 0x01U
    };
    static const uint8_t hs2_response_data[] = {
        0x01U, 0x02U, 0x03U, 0x04U
    };
    static const uint8_t expected_write[] = {
        0xAAU, 0x01U, 0x02U, 0x05U,
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U, 0x81U
    };

    fake_reset();
    perform_boot_handshake();

    fake_tick = 459U;
    AppOneWireMaster_Process();
    assert(AppOneWireMaster_GetLinkState() == APP_ONEWIRE_LINK_STALE);

    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    fake_complete_tx(460U);

    fake_tick = 461U;
    fake_push_slave_frame(hs1_response_data,
                          (uint8_t)sizeof(hs1_response_data));
    AppOneWireMaster_Process();

    fake_tick = 464U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    fake_complete_tx(465U);

    fake_tick = 466U;
    fake_push_slave_frame(hs2_response_data,
                          (uint8_t)sizeof(hs2_response_data));
    AppOneWireMaster_Process();
    assert(!fake_tx_busy);
    assert(AppOneWireMaster_GetState() ==
           APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION);

    fake_tick = 468U;
    AppOneWireMaster_Process();
    assert(!fake_tx_busy);

    fake_tick = 469U;
    AppOneWireMaster_Process();
    assert(fake_tx_busy);
    assert_tx_equals(expected_write, (uint8_t)sizeof(expected_write));
}

static void test_response_error_and_timeout(void)
{
    static const uint8_t wrong_write_response[] = {
        0x03U, 0x00U, 0x08U, 0x12U, 0x35U
    };
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    perform_boot_handshake();

    fake_tick = 12U;
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    fake_complete_tx(13U);
    fake_tick = 14U;
    fake_push_slave_frame(wrong_write_response,
                          (uint8_t)sizeof(wrong_write_response));
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_RESPONSE_ERROR);

    fake_reset();
    perform_boot_handshake();
    fake_tick = 12U;
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_READ,
               0x0008U,
               0U) == APP_ONEWIRE_SUBMIT_OK);
    AppOneWireMaster_Process();
    fake_complete_tx(13U);
    fake_tick = 113U;
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT);
}

static void test_submit_validation_and_uart_error(void)
{
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_WRITE,
               0x0008U,
               0x1234U) == APP_ONEWIRE_SUBMIT_BUSY);

    perform_boot_handshake();
    assert(AppOneWireMaster_Submit(0x55U, 0U, 0U) ==
           APP_ONEWIRE_SUBMIT_INVALID_OPERATION);
    assert(AppOneWireMaster_Submit(
               APP_ONEWIRE_OPERATION_READ,
               0x0101U,
               0U) == APP_ONEWIRE_SUBMIT_INVALID_ADDRESS);

    fake_uart_stats.uart_error_count++;
    fake_uart_stats.last_error_code = 0x08U;
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_UART_ERROR);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_UART_ERROR);
}

static void test_send_start_failure(void)
{
    AppOneWireMasterSnapshot snapshot;

    fake_reset();
    fake_send_fail_once = true;
    AppOneWireMaster_Process();
    AppOneWireMaster_GetSnapshot(&snapshot);
    assert(snapshot.link_state == APP_ONEWIRE_LINK_UART_ERROR);
    assert(snapshot.result_code == APP_ONEWIRE_RESULT_UART_ERROR);
    assert(!snapshot.busy);
}

int main(void)
{
    test_boot_handshake_and_guard();
    test_tx_done_and_response_same_process();
    test_explicit_rehandshake_honors_guard();
    test_handshake_response_timeout();
    test_write_after_handshake_guard();
    test_read_response_updates_value();
    test_stale_link_preserves_last_result();
    test_stale_write_rehandshakes_then_sends_once();
    test_response_error_and_timeout();
    test_submit_validation_and_uart_error();
    test_send_start_failure();

    puts("app_onewire_master tests passed");
    return 0;
}
