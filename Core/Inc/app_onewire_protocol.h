#ifndef APP_ONEWIRE_PROTOCOL_H
#define APP_ONEWIRE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_onewire_config.h"

typedef struct
{
    uint8_t source;
    uint8_t destination;
    uint8_t length;
    uint8_t data[APP_ONEWIRE_MAX_DATA_LEN];
} AppOneWireFrame;

typedef enum
{
    APP_ONEWIRE_PARSE_NONE = 0,
    APP_ONEWIRE_PARSE_FRAME_COMPLETE,
    APP_ONEWIRE_PARSE_FORMAT_ERROR,
    APP_ONEWIRE_PARSE_XOR_ERROR
} AppOneWireParseResult;

typedef struct
{
    uint8_t buffer[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t index;
    uint8_t expected_total;
    uint8_t running_xor;
    uint32_t last_byte_tick;
    bool receiving;
} AppOneWireParser;

void AppOneWireParser_Init(AppOneWireParser *parser);
void AppOneWireParser_Reset(AppOneWireParser *parser);

AppOneWireParseResult AppOneWireParser_Feed(
    AppOneWireParser *parser,
    uint8_t byte,
    uint32_t now_tick,
    AppOneWireFrame *frame_out);

bool AppOneWireParser_ProcessTimeout(
    AppOneWireParser *parser,
    uint32_t now_tick);

uint8_t AppOneWire_ComputeXor(const uint8_t *data, size_t length);

uint8_t AppOneWire_BuildFrame(
    uint8_t source,
    uint8_t destination,
    const uint8_t *data,
    uint8_t data_length,
    uint8_t *output,
    uint8_t output_capacity);

#endif
