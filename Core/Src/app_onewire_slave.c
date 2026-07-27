#include "app_onewire_slave.h"

#include <stddef.h>
#include <string.h>

#include "app_onewire_config.h"
#include "app_onewire_protocol.h"
#include "app_onewire_uart.h"
#include "stm32f4xx_hal.h"

typedef enum
{
    SLAVE_POST_TX_NONE = 0,
    SLAVE_POST_TX_WAIT_HANDSHAKE_2,
    SLAVE_POST_TX_ONLINE
} SlavePostTxAction;

static const uint8_t handshake_data_1[4] = {
    0x01U, 0x02U, 0x03U, 0x04U
};

static const uint8_t handshake_data_2[4] = {
    0x04U, 0x03U, 0x02U, 0x01U
};

static AppOneWireParser parser;
static AppOneWireSlaveState slave_state;
static uint16_t register_map[APP_ONEWIRE_REGISTER_MAX_ADDR + 1U];

static uint8_t response_buffer[APP_ONEWIRE_MAX_FRAME_LEN];
static uint8_t response_length;
static bool response_pending;
static bool response_in_flight;
static uint32_t response_queued_tick;
static SlavePostTxAction response_action;

static uint32_t handshake_start_tick;
static uint32_t last_valid_request_tick;
static AppOneWireSlaveStats slave_stats;

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

static bool frame_is_for_slave(const AppOneWireFrame *frame)
{
    return (frame->source == APP_ONEWIRE_LOCAL_ADDR_MASTER) &&
           (frame->destination == APP_ONEWIRE_LOCAL_ADDR_SLAVE);
}

static bool frame_is_handshake_1(const AppOneWireFrame *frame)
{
    return (frame->length == sizeof(handshake_data_1)) &&
           data_matches(frame->data,
                        handshake_data_1,
                        (uint8_t)sizeof(handshake_data_1));
}

static bool frame_is_handshake_2(const AppOneWireFrame *frame)
{
    return (frame->length == sizeof(handshake_data_2)) &&
           data_matches(frame->data,
                        handshake_data_2,
                        (uint8_t)sizeof(handshake_data_2));
}

static bool queue_response(
    const uint8_t *data,
    uint8_t data_length,
    SlavePostTxAction action,
    uint32_t now_tick)
{
    uint8_t length;

    if (response_pending || response_in_flight)
    {
        slave_stats.response_busy_drop_count++;
        return false;
    }

    length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_SLAVE,
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        data,
        data_length,
        response_buffer,
        (uint8_t)sizeof(response_buffer));

    if (length == 0U)
    {
        slave_stats.response_send_fail_count++;
        return false;
    }

    response_length = length;
    response_action = action;
    response_queued_tick = now_tick;
    response_pending = true;
    slave_stats.response_queued_count++;
    return true;
}

static void execute_post_tx_action(void)
{
    SlavePostTxAction action = response_action;

    response_action = SLAVE_POST_TX_NONE;

    if (action == SLAVE_POST_TX_WAIT_HANDSHAKE_2)
    {
        slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2;
    }
    else if (action == SLAVE_POST_TX_ONLINE)
    {
        slave_state = APP_ONEWIRE_SLAVE_ONLINE;
    }
}

static void process_tx_completion(void)
{
    if (!AppOneWireUart_TakeTxDone())
    {
        return;
    }

    if (!response_in_flight)
    {
        return;
    }

    response_in_flight = false;
    slave_stats.response_sent_count++;
    execute_post_tx_action();
}

static void flush_response(uint32_t now_tick)
{
    if (!response_pending || response_in_flight)
    {
        return;
    }

    if ((uint32_t)(now_tick - response_queued_tick) <
        APP_ONEWIRE_RESPONSE_TURNAROUND_MS)
    {
        return;
    }

    if (AppOneWireUart_IsTxBusy())
    {
        return;
    }

    if (!AppOneWireUart_Send(response_buffer, response_length))
    {
        slave_stats.response_send_fail_count++;
        return;
    }

    response_pending = false;
    response_in_flight = true;
}

static void begin_handshake(uint32_t now_tick)
{
    if (!queue_response(
            handshake_data_2,
            (uint8_t)sizeof(handshake_data_2),
            SLAVE_POST_TX_WAIT_HANDSHAKE_2,
            now_tick))
    {
        return;
    }

    handshake_start_tick = now_tick;
    slave_stats.handshake_1_count++;
}

static void handle_handshake_2(uint32_t now_tick)
{
    if ((uint32_t)(now_tick - handshake_start_tick) >=
        APP_ONEWIRE_HANDSHAKE_TOTAL_MS)
    {
        slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1;
        slave_stats.handshake_timeout_count++;
        return;
    }

    if (!queue_response(
            handshake_data_1,
            (uint8_t)sizeof(handshake_data_1),
            SLAVE_POST_TX_ONLINE,
            now_tick))
    {
        return;
    }

    last_valid_request_tick = now_tick;
    slave_stats.handshake_2_count++;
}

static void handle_write(const AppOneWireFrame *frame, uint32_t now_tick)
{
    uint16_t address;
    uint16_t value;

    address = (uint16_t)(((uint16_t)frame->data[1] << 8U) |
                         frame->data[2]);
    value = (uint16_t)(((uint16_t)frame->data[3] << 8U) |
                       frame->data[4]);

    if (address > APP_ONEWIRE_REGISTER_MAX_ADDR)
    {
        slave_stats.invalid_address_count++;
        return;
    }

    if (!queue_response(frame->data,
                        frame->length,
                        SLAVE_POST_TX_NONE,
                        now_tick))
    {
        return;
    }

    register_map[address] = value;
    last_valid_request_tick = now_tick;
    slave_stats.write_count++;
}

static void handle_read(const AppOneWireFrame *frame, uint32_t now_tick)
{
    uint8_t response_data[5];
    uint16_t address;
    uint16_t value;

    address = (uint16_t)(((uint16_t)frame->data[1] << 8U) |
                         frame->data[2]);

    if (address > APP_ONEWIRE_REGISTER_MAX_ADDR)
    {
        slave_stats.invalid_address_count++;
        return;
    }

    value = register_map[address];
    response_data[0] = APP_ONEWIRE_OPERATION_READ;
    response_data[1] = frame->data[1];
    response_data[2] = frame->data[2];
    response_data[3] = (uint8_t)(value >> 8U);
    response_data[4] = (uint8_t)value;

    if (!queue_response(response_data,
                        (uint8_t)sizeof(response_data),
                        SLAVE_POST_TX_NONE,
                        now_tick))
    {
        return;
    }

    last_valid_request_tick = now_tick;
    slave_stats.read_count++;
}

static void process_online_frame(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    if (frame->length != 5U)
    {
        slave_stats.invalid_frame_count++;
        return;
    }

    if (frame->data[0] == APP_ONEWIRE_OPERATION_WRITE)
    {
        handle_write(frame, now_tick);
    }
    else if (frame->data[0] == APP_ONEWIRE_OPERATION_READ)
    {
        if ((frame->data[3] != 0U) || (frame->data[4] != 0U))
        {
            slave_stats.invalid_frame_count++;
            return;
        }

        handle_read(frame, now_tick);
    }
    else
    {
        slave_stats.invalid_operation_count++;
    }
}

static void process_frame(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    if (!frame_is_for_slave(frame))
    {
        slave_stats.invalid_frame_count++;
        return;
    }

    if (response_pending || response_in_flight)
    {
        slave_stats.response_busy_drop_count++;
        return;
    }

    if (frame_is_handshake_1(frame))
    {
        begin_handshake(now_tick);
        return;
    }

    if ((slave_state == APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2) &&
        frame_is_handshake_2(frame))
    {
        handle_handshake_2(now_tick);
        return;
    }

    if (slave_state == APP_ONEWIRE_SLAVE_ONLINE)
    {
        process_online_frame(frame, now_tick);
        return;
    }

    slave_stats.invalid_frame_count++;
}

static void process_rx(uint32_t now_tick)
{
    uint8_t byte;
    AppOneWireFrame frame;

    while (AppOneWireUart_ReadByte(&byte))
    {
        AppOneWireParseResult result = AppOneWireParser_Feed(
            &parser, byte, now_tick, &frame);

        if (result == APP_ONEWIRE_PARSE_FRAME_COMPLETE)
        {
            slave_stats.parser_frame_count++;
            process_frame(&frame, now_tick);
        }
        else if (result == APP_ONEWIRE_PARSE_XOR_ERROR)
        {
            slave_stats.parser_xor_error_count++;
        }
        else if (result == APP_ONEWIRE_PARSE_FORMAT_ERROR)
        {
            slave_stats.parser_format_error_count++;
        }
    }
}

static void process_state_timeouts(uint32_t now_tick)
{
    if (response_pending || response_in_flight)
    {
        return;
    }

    if (slave_state == APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2)
    {
        if ((uint32_t)(now_tick - handshake_start_tick) >=
            APP_ONEWIRE_HANDSHAKE_TOTAL_MS)
        {
            slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1;
            slave_stats.handshake_timeout_count++;
        }
    }
    else if (slave_state == APP_ONEWIRE_SLAVE_ONLINE)
    {
        if ((uint32_t)(now_tick - last_valid_request_tick) >=
            APP_ONEWIRE_SLAVE_FAULT_MS)
        {
            slave_state = APP_ONEWIRE_SLAVE_COMM_FAULT;
            slave_stats.comm_fault_count++;
        }
    }
}

void AppOneWireSlave_Init(void)
{
    AppOneWireParser_Init(&parser);
    (void)memset(register_map, 0, sizeof(register_map));
    (void)memset(&slave_stats, 0, sizeof(slave_stats));

    slave_state = APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1;
    response_length = 0U;
    response_pending = false;
    response_in_flight = false;
    response_queued_tick = 0U;
    response_action = SLAVE_POST_TX_NONE;
    handshake_start_tick = 0U;
    last_valid_request_tick = 0U;
}

void AppOneWireSlave_Process(void)
{
    uint32_t now_tick = HAL_GetTick();

    if (AppOneWireParser_ProcessTimeout(&parser, now_tick))
    {
        slave_stats.parser_timeout_count++;
    }

    process_tx_completion();
    process_rx(now_tick);
    flush_response(now_tick);
    process_state_timeouts(now_tick);
}

AppOneWireSlaveState AppOneWireSlave_GetState(void)
{
    return slave_state;
}

bool AppOneWireSlave_IsResponsePending(void)
{
    return response_pending || response_in_flight;
}

bool AppOneWireSlave_ReadRegister(uint16_t address, uint16_t *value)
{
    if ((value == NULL) ||
        (address > APP_ONEWIRE_REGISTER_MAX_ADDR))
    {
        return false;
    }

    *value = register_map[address];
    return true;
}

void AppOneWireSlave_GetStats(AppOneWireSlaveStats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    *stats = slave_stats;
}
