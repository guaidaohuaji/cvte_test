#include "app_onewire_protocol.h"

#include <string.h>

static void parser_start_frame(AppOneWireParser *parser, uint32_t now_tick)
{
    parser->buffer[0] = APP_ONEWIRE_FRAME_HEADER;
    parser->index = 1U;
    parser->expected_total = 0U;
    parser->running_xor = APP_ONEWIRE_FRAME_HEADER;
    parser->last_byte_tick = now_tick;
    parser->receiving = true;
}

void AppOneWireParser_Init(AppOneWireParser *parser)
{
    AppOneWireParser_Reset(parser);
}

void AppOneWireParser_Reset(AppOneWireParser *parser)
{
    if (parser == NULL)
    {
        return;
    }

    parser->index = 0U;
    parser->expected_total = 0U;
    parser->running_xor = 0U;
    parser->last_byte_tick = 0U;
    parser->receiving = false;
}

bool AppOneWireParser_ProcessTimeout(
    AppOneWireParser *parser,
    uint32_t now_tick)
{
    if ((parser == NULL) || !parser->receiving)
    {
        return false;
    }

    if ((uint32_t)(now_tick - parser->last_byte_tick) <
        APP_ONEWIRE_INTERBYTE_TIMEOUT_MS)
    {
        return false;
    }

    AppOneWireParser_Reset(parser);
    return true;
}

AppOneWireParseResult AppOneWireParser_Feed(
    AppOneWireParser *parser,
    uint8_t byte,
    uint32_t now_tick,
    AppOneWireFrame *frame_out)
{
    if ((parser == NULL) || (frame_out == NULL))
    {
        return APP_ONEWIRE_PARSE_FORMAT_ERROR;
    }

    (void)AppOneWireParser_ProcessTimeout(parser, now_tick);

    if (!parser->receiving)
    {
        if (byte == APP_ONEWIRE_FRAME_HEADER)
        {
            parser_start_frame(parser, now_tick);
        }
        return APP_ONEWIRE_PARSE_NONE;
    }

    if (parser->index >= APP_ONEWIRE_MAX_FRAME_LEN)
    {
        AppOneWireParser_Reset(parser);
        if (byte == APP_ONEWIRE_FRAME_HEADER)
        {
            parser_start_frame(parser, now_tick);
        }
        return APP_ONEWIRE_PARSE_FORMAT_ERROR;
    }

    parser->buffer[parser->index] = byte;
    parser->last_byte_tick = now_tick;

    if (parser->index == 3U)
    {
        if (byte > APP_ONEWIRE_MAX_DATA_LEN)
        {
            AppOneWireParser_Reset(parser);
            if (byte == APP_ONEWIRE_FRAME_HEADER)
            {
                parser_start_frame(parser, now_tick);
            }
            return APP_ONEWIRE_PARSE_FORMAT_ERROR;
        }

        parser->expected_total =
            (uint8_t)(APP_ONEWIRE_FRAME_OVERHEAD + byte);
    }

    if ((parser->expected_total != 0U) &&
        (parser->index == (uint8_t)(parser->expected_total - 1U)))
    {
        AppOneWireParseResult result;

        if (byte != parser->running_xor)
        {
            result = APP_ONEWIRE_PARSE_XOR_ERROR;
        }
        else
        {
            frame_out->source = parser->buffer[1];
            frame_out->destination = parser->buffer[2];
            frame_out->length = parser->buffer[3];

            if (frame_out->length > 0U)
            {
                memcpy(frame_out->data,
                       &parser->buffer[4],
                       frame_out->length);
            }

            result = APP_ONEWIRE_PARSE_FRAME_COMPLETE;
        }

        AppOneWireParser_Reset(parser);
        if ((result != APP_ONEWIRE_PARSE_FRAME_COMPLETE) &&
            (byte == APP_ONEWIRE_FRAME_HEADER))
        {
            parser_start_frame(parser, now_tick);
        }
        return result;
    }

    parser->running_xor ^= byte;
    parser->index++;
    return APP_ONEWIRE_PARSE_NONE;
}

uint8_t AppOneWire_ComputeXor(const uint8_t *data, size_t length)
{
    uint8_t value = 0U;
    size_t index;

    if ((data == NULL) && (length != 0U))
    {
        return 0U;
    }

    for (index = 0U; index < length; index++)
    {
        value ^= data[index];
    }

    return value;
}

uint8_t AppOneWire_BuildFrame(
    uint8_t source,
    uint8_t destination,
    const uint8_t *data,
    uint8_t data_length,
    uint8_t *output,
    uint8_t output_capacity)
{
    uint8_t total_length;

    if ((output == NULL) ||
        (data_length > APP_ONEWIRE_MAX_DATA_LEN) ||
        ((data == NULL) && (data_length != 0U)))
    {
        return 0U;
    }

    total_length =
        (uint8_t)(APP_ONEWIRE_FRAME_OVERHEAD + data_length);

    if (output_capacity < total_length)
    {
        return 0U;
    }

    output[0] = APP_ONEWIRE_FRAME_HEADER;
    output[1] = source;
    output[2] = destination;
    output[3] = data_length;

    if (data_length > 0U)
    {
        memcpy(&output[4], data, data_length);
    }

    output[total_length - 1U] =
        AppOneWire_ComputeXor(output, total_length - 1U);

    return total_length;
}
