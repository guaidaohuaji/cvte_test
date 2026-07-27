#include "app_onewire_master.h"

#include <stddef.h>
#include "app_onewire_config.h"
#include "app_onewire_protocol.h"
#include "app_onewire_uart.h"
#include "stm32f4xx_hal.h"

static const uint8_t handshake_data_1[4] = {
    0x01U, 0x02U, 0x03U, 0x04U
};

static const uint8_t handshake_data_2[4] = {
    0x04U, 0x03U, 0x02U, 0x01U
};

static AppOneWireParser parser;
static AppOneWireMasterState master_state;
static AppOneWireLinkState link_state;
static AppOneWireResultCode result_code;
static AppOneWireMasterStats master_stats;

static uint8_t tx_frame[APP_ONEWIRE_MAX_FRAME_LEN];
static bool busy_flag;
static bool pending_valid;
static uint8_t pending_operation;
static uint16_t pending_address;
static uint16_t pending_value;

static uint8_t last_operation;
static uint16_t last_address;
static uint16_t last_value;

static uint32_t handshake_start_tick;
static uint32_t tx_start_tick;
static uint32_t response_wait_start_tick;
static uint32_t last_valid_rx_tick;
static bool last_valid_rx_tick_valid;
static uint32_t observed_uart_error_count;

static bool data_matches(
    const uint8_t *actual,
    const uint8_t *expected,
    uint8_t length)
{
    uint8_t index;

    for (index = 0U; index < length; ++index)
    {
        if (actual[index] != expected[index])
        {
            return false;
        }
    }

    return true;
}

static bool frame_is_from_slave(const AppOneWireFrame *frame)
{
    return (frame->source == APP_ONEWIRE_LOCAL_ADDR_SLAVE) &&
           (frame->destination == APP_ONEWIRE_LOCAL_ADDR_MASTER);
}

static bool frame_has_data(
    const AppOneWireFrame *frame,
    const uint8_t *data,
    uint8_t length)
{
    return (frame->length == length) &&
           data_matches(frame->data, data, length);
}

static void reset_pending(void)
{
    pending_valid = false;
    pending_operation = 0U;
    pending_address = 0U;
    pending_value = 0U;
}

static void drain_rx_and_reset_parser(void)
{
    uint8_t byte;

    while (AppOneWireUart_ReadByte(&byte))
    {
        /* Discard stale bytes before starting a new transaction. */
    }
    AppOneWireParser_Reset(&parser);
}

static bool link_is_fresh(uint32_t now_tick)
{
    return (link_state == APP_ONEWIRE_LINK_ONLINE) &&
           last_valid_rx_tick_valid &&
           ((uint32_t)(now_tick - last_valid_rx_tick) <
            APP_ONEWIRE_MASTER_LINK_VALID_MS);
}

static bool guard_has_elapsed(uint32_t now_tick)
{
    return !last_valid_rx_tick_valid ||
           ((uint32_t)(now_tick - last_valid_rx_tick) >=
            APP_ONEWIRE_MASTER_GUARD_MS);
}

static void mark_valid_response(uint32_t now_tick)
{
    last_valid_rx_tick = now_tick;
    last_valid_rx_tick_valid = true;
}

static void finish_success(uint16_t value, uint32_t now_tick)
{
    last_value = value;
    result_code = APP_ONEWIRE_RESULT_SUCCESS;
    link_state = APP_ONEWIRE_LINK_ONLINE;
    mark_valid_response(now_tick);
    reset_pending();
    busy_flag = false;
    master_state = APP_ONEWIRE_MASTER_ONLINE_IDLE;
}

static void fail_handshake(bool timed_out)
{
    if (timed_out)
    {
        master_stats.handshake_timeout_count++;
    }
    result_code = APP_ONEWIRE_RESULT_HANDSHAKE_FAILED;
    link_state = APP_ONEWIRE_LINK_STALE;
    reset_pending();
    busy_flag = false;
    master_state = APP_ONEWIRE_MASTER_ERROR_IDLE;
}

static void fail_response(AppOneWireResultCode code)
{
    if (code == APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT)
    {
        master_stats.operation_timeout_count++;
    }
    else if (code == APP_ONEWIRE_RESULT_RESPONSE_ERROR)
    {
        master_stats.response_error_count++;
    }

    result_code = code;
    link_state = APP_ONEWIRE_LINK_STALE;
    reset_pending();
    busy_flag = false;
    master_state = APP_ONEWIRE_MASTER_ERROR_IDLE;
}

static void fail_uart(void)
{
    master_stats.uart_error_count++;
    result_code = APP_ONEWIRE_RESULT_UART_ERROR;
    link_state = APP_ONEWIRE_LINK_UART_ERROR;
    reset_pending();
    busy_flag = false;
    master_state = APP_ONEWIRE_MASTER_ERROR_IDLE;
}

static bool start_frame_tx(
    const uint8_t *data,
    uint8_t data_length,
    AppOneWireMasterState tx_state,
    uint32_t now_tick)
{
    uint8_t frame_length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        APP_ONEWIRE_LOCAL_ADDR_SLAVE,
        data,
        data_length,
        tx_frame,
        (uint8_t)sizeof(tx_frame));

    if ((frame_length == 0U) ||
        !AppOneWireUart_Send(tx_frame, frame_length))
    {
        fail_uart();
        return false;
    }

    tx_start_tick = now_tick;
    master_state = tx_state;
    return true;
}

static void begin_handshake(uint32_t now_tick)
{
    drain_rx_and_reset_parser();
    link_state = APP_ONEWIRE_LINK_HANDSHAKING;
    handshake_start_tick = now_tick;
    master_stats.handshake_attempt_count++;

    (void)start_frame_tx(
        handshake_data_1,
        (uint8_t)sizeof(handshake_data_1),
        APP_ONEWIRE_MASTER_HANDSHAKE_1_TX,
        now_tick);
}

static void start_pending_operation(uint32_t now_tick)
{
    uint8_t data[5];

    if (!pending_valid)
    {
        busy_flag = false;
        master_state = APP_ONEWIRE_MASTER_ONLINE_IDLE;
        return;
    }

    data[0] = pending_operation;
    data[1] = (uint8_t)(pending_address >> 8U);
    data[2] = (uint8_t)pending_address;

    if (pending_operation == APP_ONEWIRE_OPERATION_WRITE)
    {
        data[3] = (uint8_t)(pending_value >> 8U);
        data[4] = (uint8_t)pending_value;
        if (start_frame_tx(data,
                           (uint8_t)sizeof(data),
                           APP_ONEWIRE_MASTER_WRITE_TX,
                           now_tick))
        {
            master_stats.write_tx_count++;
        }
    }
    else if (pending_operation == APP_ONEWIRE_OPERATION_READ)
    {
        data[3] = 0U;
        data[4] = 0U;
        if (start_frame_tx(data,
                           (uint8_t)sizeof(data),
                           APP_ONEWIRE_MASTER_READ_TX,
                           now_tick))
        {
            master_stats.read_tx_count++;
        }
    }
    else
    {
        fail_response(APP_ONEWIRE_RESULT_RESPONSE_ERROR);
    }
}

static void complete_handshake(uint32_t now_tick)
{
    master_stats.handshake_2_ok_count++;
    link_state = APP_ONEWIRE_LINK_ONLINE;
    mark_valid_response(now_tick);

    if (pending_valid &&
        (pending_operation == APP_ONEWIRE_OPERATION_REHANDSHAKE))
    {
        result_code = APP_ONEWIRE_RESULT_SUCCESS;
        reset_pending();
        busy_flag = false;
        master_state = APP_ONEWIRE_MASTER_ONLINE_IDLE;
    }
    else if (pending_valid)
    {
        master_state = APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION;
    }
    else
    {
        result_code = APP_ONEWIRE_RESULT_SUCCESS;
        busy_flag = false;
        master_state = APP_ONEWIRE_MASTER_ONLINE_IDLE;
    }
}

static void handle_handshake_1_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    if (!frame_is_from_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(now_tick);

    if (!frame_has_data(frame,
                        handshake_data_2,
                        (uint8_t)sizeof(handshake_data_2)))
    {
        master_stats.response_error_count++;
        fail_handshake(false);
        return;
    }

    master_stats.handshake_1_ok_count++;
    master_state = APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2;
}

static void handle_handshake_2_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    if (!frame_is_from_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(now_tick);

    if (!frame_has_data(frame,
                        handshake_data_1,
                        (uint8_t)sizeof(handshake_data_1)))
    {
        master_stats.response_error_count++;
        fail_handshake(false);
        return;
    }

    complete_handshake(now_tick);
}

static void handle_write_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    uint8_t expected[5];

    if (!frame_is_from_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(now_tick);

    expected[0] = APP_ONEWIRE_OPERATION_WRITE;
    expected[1] = (uint8_t)(last_address >> 8U);
    expected[2] = (uint8_t)last_address;
    expected[3] = (uint8_t)(last_value >> 8U);
    expected[4] = (uint8_t)last_value;

    if (!frame_has_data(frame, expected, (uint8_t)sizeof(expected)))
    {
        fail_response(APP_ONEWIRE_RESULT_RESPONSE_ERROR);
        return;
    }

    master_stats.write_ok_count++;
    finish_success(last_value, now_tick);
}

static void handle_read_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    uint16_t value;

    if (!frame_is_from_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(now_tick);

    if ((frame->length != 5U) ||
        (frame->data[0] != APP_ONEWIRE_OPERATION_READ) ||
        (frame->data[1] != (uint8_t)(last_address >> 8U)) ||
        (frame->data[2] != (uint8_t)last_address))
    {
        fail_response(APP_ONEWIRE_RESULT_RESPONSE_ERROR);
        return;
    }

    value = (uint16_t)(((uint16_t)frame->data[3] << 8U) |
                       frame->data[4]);
    master_stats.read_ok_count++;
    finish_success(value, now_tick);
}

static void process_complete_frame(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    master_stats.parser_frame_count++;

    if (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT)
    {
        handle_handshake_1_response(frame, now_tick);
    }
    else if (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT)
    {
        handle_handshake_2_response(frame, now_tick);
    }
    else if (master_state == APP_ONEWIRE_MASTER_WRITE_WAIT)
    {
        handle_write_response(frame, now_tick);
    }
    else if (master_state == APP_ONEWIRE_MASTER_READ_WAIT)
    {
        handle_read_response(frame, now_tick);
    }
    else
    {
        master_stats.unexpected_frame_count++;
    }
}

static void process_rx(uint32_t now_tick)
{
    uint8_t byte;
    AppOneWireFrame frame;

    while (AppOneWireUart_ReadByte(&byte))
    {
        AppOneWireParseResult parse_result = AppOneWireParser_Feed(
            &parser, byte, now_tick, &frame);

        if (parse_result == APP_ONEWIRE_PARSE_FRAME_COMPLETE)
        {
            process_complete_frame(&frame, now_tick);
        }
        else if (parse_result == APP_ONEWIRE_PARSE_XOR_ERROR)
        {
            master_stats.parser_xor_error_count++;
        }
        else if (parse_result == APP_ONEWIRE_PARSE_FORMAT_ERROR)
        {
            master_stats.parser_format_error_count++;
        }
    }
}

static bool state_is_handshaking(void)
{
    return (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_TX) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT) ||
           (master_state == APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_TX) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT);
}

static void process_uart_error_event(void)
{
    AppOneWireUartStats uart_stats;

    AppOneWireUart_GetStats(&uart_stats);
    if (uart_stats.uart_error_count == observed_uart_error_count)
    {
        return;
    }

    observed_uart_error_count = uart_stats.uart_error_count;
    fail_uart();
}

static void process_tx_state(uint32_t now_tick)
{
    AppOneWireMasterState wait_state;

    if (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_TX)
    {
        wait_state = APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT;
    }
    else if (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_TX)
    {
        wait_state = APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT;
    }
    else if (master_state == APP_ONEWIRE_MASTER_WRITE_TX)
    {
        wait_state = APP_ONEWIRE_MASTER_WRITE_WAIT;
    }
    else
    {
        wait_state = APP_ONEWIRE_MASTER_READ_WAIT;
    }

    if (AppOneWireUart_TakeTxDone())
    {
        response_wait_start_tick = now_tick;
        AppOneWireParser_Reset(&parser);
        master_state = wait_state;
        return;
    }

    if ((uint32_t)(now_tick - tx_start_tick) >=
        APP_ONEWIRE_TX_COMPLETE_TIMEOUT_MS)
    {
        master_stats.tx_timeout_count++;
        if (state_is_handshaking())
        {
            fail_handshake(true);
        }
        else
        {
            fail_uart();
        }
    }
}

static void process_wait_timeout(uint32_t now_tick)
{
    if ((master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT) ||
        (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT))
    {
        if ((uint32_t)(now_tick - response_wait_start_tick) >=
            APP_ONEWIRE_HANDSHAKE_RESPONSE_MS)
        {
            fail_handshake(true);
        }
    }
    else if ((master_state == APP_ONEWIRE_MASTER_WRITE_WAIT) ||
             (master_state == APP_ONEWIRE_MASTER_READ_WAIT))
    {
        if ((uint32_t)(now_tick - response_wait_start_tick) >=
            APP_ONEWIRE_OPERATION_RESPONSE_MS)
        {
            fail_response(APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT);
        }
    }
}

static void process_handshake_total_timeout(uint32_t now_tick)
{
    if (state_is_handshaking() &&
        ((uint32_t)(now_tick - handshake_start_tick) >=
         APP_ONEWIRE_HANDSHAKE_TOTAL_MS))
    {
        fail_handshake(true);
    }
}

void AppOneWireMaster_Init(void)
{
    AppOneWireUartStats uart_stats;

    AppOneWireParser_Init(&parser);
    master_stats = (AppOneWireMasterStats){0};

    master_state = APP_ONEWIRE_MASTER_BOOT;
    link_state = APP_ONEWIRE_LINK_OFFLINE;
    result_code = APP_ONEWIRE_RESULT_PENDING;
    busy_flag = true;
    reset_pending();

    last_operation = APP_ONEWIRE_OPERATION_REHANDSHAKE;
    last_address = 0U;
    last_value = 0U;

    handshake_start_tick = 0U;
    tx_start_tick = 0U;
    response_wait_start_tick = 0U;
    last_valid_rx_tick = 0U;
    last_valid_rx_tick_valid = false;

    AppOneWireUart_GetStats(&uart_stats);
    observed_uart_error_count = uart_stats.uart_error_count;
}

void AppOneWireMaster_Process(void)
{
    uint32_t now_tick = HAL_GetTick();

    if (AppOneWireParser_ProcessTimeout(&parser, now_tick))
    {
        master_stats.parser_timeout_count++;
    }

    process_uart_error_event();

    /* Consume TX completion before RX so an already queued response is
     * parsed in the matching WAIT state rather than discarded in TX state. */
    if ((master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_TX) ||
        (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_TX) ||
        (master_state == APP_ONEWIRE_MASTER_WRITE_TX) ||
        (master_state == APP_ONEWIRE_MASTER_READ_TX))
    {
        process_tx_state(now_tick);
    }

    process_rx(now_tick);

    if (master_state == APP_ONEWIRE_MASTER_BOOT)
    {
        master_state = APP_ONEWIRE_MASTER_HANDSHAKE_START;
    }

    if (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_START)
    {
        if (guard_has_elapsed(now_tick))
        {
            begin_handshake(now_tick);
        }
    }
    else if (master_state == APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2)
    {
        if (guard_has_elapsed(now_tick))
        {
            (void)start_frame_tx(
                handshake_data_2,
                (uint8_t)sizeof(handshake_data_2),
                APP_ONEWIRE_MASTER_HANDSHAKE_2_TX,
                now_tick);
        }
    }
    else if (master_state == APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION)
    {
        if (guard_has_elapsed(now_tick))
        {
            start_pending_operation(now_tick);
        }
    }
    else if (master_state == APP_ONEWIRE_MASTER_ONLINE_IDLE)
    {
        if (!link_is_fresh(now_tick))
        {
            link_state = APP_ONEWIRE_LINK_STALE;
            master_state = APP_ONEWIRE_MASTER_STALE_IDLE;
        }
    }

    process_wait_timeout(now_tick);
    process_handshake_total_timeout(now_tick);
}

AppOneWireSubmitResult AppOneWireMaster_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    uint32_t now_tick;

    if (busy_flag)
    {
        return APP_ONEWIRE_SUBMIT_BUSY;
    }

    if ((operation != APP_ONEWIRE_OPERATION_REHANDSHAKE) &&
        (operation != APP_ONEWIRE_OPERATION_WRITE) &&
        (operation != APP_ONEWIRE_OPERATION_READ))
    {
        return APP_ONEWIRE_SUBMIT_INVALID_OPERATION;
    }

    if ((operation != APP_ONEWIRE_OPERATION_REHANDSHAKE) &&
        (address > APP_ONEWIRE_REGISTER_MAX_ADDR))
    {
        return APP_ONEWIRE_SUBMIT_INVALID_ADDRESS;
    }

    if (operation == APP_ONEWIRE_OPERATION_REHANDSHAKE)
    {
        address = 0U;
        value = 0U;
    }

    pending_valid = true;
    pending_operation = operation;
    pending_address = address;
    pending_value = value;

    last_operation = operation;
    last_address = address;
    last_value = value;
    result_code = APP_ONEWIRE_RESULT_PENDING;
    busy_flag = true;

    now_tick = HAL_GetTick();
    if ((operation == APP_ONEWIRE_OPERATION_REHANDSHAKE) ||
        !link_is_fresh(now_tick))
    {
        master_state = APP_ONEWIRE_MASTER_HANDSHAKE_START;
    }
    else
    {
        master_state = APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION;
    }

    return APP_ONEWIRE_SUBMIT_OK;
}

void AppOneWireMaster_GetSnapshot(AppOneWireMasterSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    snapshot->link_state = link_state;
    snapshot->master_state = master_state;
    snapshot->busy = busy_flag;
    snapshot->pending_valid = pending_valid;
    snapshot->last_operation = last_operation;
    snapshot->result_code = result_code;
    snapshot->address = last_address;
    snapshot->value = last_value;
}

void AppOneWireMaster_GetStats(AppOneWireMasterStats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    *stats = master_stats;
}

AppOneWireMasterState AppOneWireMaster_GetState(void)
{
    return master_state;
}

AppOneWireLinkState AppOneWireMaster_GetLinkState(void)
{
    return link_state;
}

bool AppOneWireMaster_IsBusy(void)
{
    return busy_flag;
}
