#include "app_onewire_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void assert_bytes(
    const uint8_t *actual,
    const uint8_t *expected,
    size_t length)
{
    assert(memcmp(actual, expected, length) == 0);
}

static void test_build_known_frames(void)
{
    uint8_t output[APP_ONEWIRE_MAX_FRAME_LEN];
    const uint8_t hs1_data[] = {0x01U, 0x02U, 0x03U, 0x04U};
    const uint8_t hs1_expected[] = {
        0xAAU, 0x01U, 0x02U, 0x04U,
        0x01U, 0x02U, 0x03U, 0x04U, 0xA9U
    };
    const uint8_t write_data[] = {
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U
    };
    const uint8_t write_expected[] = {
        0xAAU, 0x01U, 0x02U, 0x05U,
        0x03U, 0x00U, 0x08U, 0x12U, 0x34U, 0x81U
    };
    uint8_t length;

    length = AppOneWire_BuildFrame(
        0x01U, 0x02U, hs1_data, sizeof(hs1_data),
        output, sizeof(output));
    assert(length == sizeof(hs1_expected));
    assert_bytes(output, hs1_expected, sizeof(hs1_expected));

    length = AppOneWire_BuildFrame(
        0x01U, 0x02U, write_data, sizeof(write_data),
        output, sizeof(output));
    assert(length == sizeof(write_expected));
    assert_bytes(output, write_expected, sizeof(write_expected));
}

static void test_parse_known_frame(void)
{
    const uint8_t frame_bytes[] = {
        0xAAU, 0x02U, 0x01U, 0x05U,
        0x06U, 0x00U, 0x08U, 0x12U, 0x34U, 0x84U
    };
    AppOneWireParser parser;
    AppOneWireFrame frame;
    AppOneWireParseResult result = APP_ONEWIRE_PARSE_NONE;
    size_t index;

    AppOneWireParser_Init(&parser);

    for (index = 0U; index < sizeof(frame_bytes); index++)
    {
        result = AppOneWireParser_Feed(
            &parser, frame_bytes[index], (uint32_t)index, &frame);
    }

    assert(result == APP_ONEWIRE_PARSE_FRAME_COMPLETE);
    assert(frame.source == 0x02U);
    assert(frame.destination == 0x01U);
    assert(frame.length == 5U);
    assert(frame.data[0] == 0x06U);
    assert(frame.data[1] == 0x00U);
    assert(frame.data[2] == 0x08U);
    assert(frame.data[3] == 0x12U);
    assert(frame.data[4] == 0x34U);
}

static void test_xor_error(void)
{
    const uint8_t frame_bytes[] = {
        0xAAU, 0x01U, 0x02U, 0x04U,
        0x01U, 0x02U, 0x03U, 0x04U, 0x00U
    };
    AppOneWireParser parser;
    AppOneWireFrame frame;
    AppOneWireParseResult result = APP_ONEWIRE_PARSE_NONE;
    size_t index;

    AppOneWireParser_Init(&parser);

    for (index = 0U; index < sizeof(frame_bytes); index++)
    {
        result = AppOneWireParser_Feed(
            &parser, frame_bytes[index], (uint32_t)index, &frame);
    }

    assert(result == APP_ONEWIRE_PARSE_XOR_ERROR);
}

static void test_format_error_and_recovery(void)
{
    AppOneWireParser parser;
    AppOneWireFrame frame;
    AppOneWireParseResult result;
    const uint8_t valid[] = {
        0xAAU, 0x01U, 0x02U, 0x00U, 0xA9U
    };
    size_t index;

    AppOneWireParser_Init(&parser);

    assert(AppOneWireParser_Feed(
        &parser, 0xAAU, 0U, &frame) == APP_ONEWIRE_PARSE_NONE);
    assert(AppOneWireParser_Feed(
        &parser, 0x01U, 1U, &frame) == APP_ONEWIRE_PARSE_NONE);
    assert(AppOneWireParser_Feed(
        &parser, 0x02U, 2U, &frame) == APP_ONEWIRE_PARSE_NONE);
    result = AppOneWireParser_Feed(
        &parser, 0x21U, 3U, &frame);
    assert(result == APP_ONEWIRE_PARSE_FORMAT_ERROR);

    result = APP_ONEWIRE_PARSE_NONE;
    for (index = 0U; index < sizeof(valid); index++)
    {
        result = AppOneWireParser_Feed(
            &parser, valid[index], (uint32_t)(10U + index), &frame);
    }
    assert(result == APP_ONEWIRE_PARSE_FRAME_COMPLETE);
    assert(frame.length == 0U);
}

static void test_interbyte_timeout(void)
{
    AppOneWireParser parser;
    AppOneWireFrame frame;

    AppOneWireParser_Init(&parser);
    assert(AppOneWireParser_Feed(
        &parser, 0xAAU, 100U, &frame) == APP_ONEWIRE_PARSE_NONE);
    assert(AppOneWireParser_Feed(
        &parser, 0x01U, 101U, &frame) == APP_ONEWIRE_PARSE_NONE);
    assert(AppOneWireParser_ProcessTimeout(
        &parser, 105U) == false);
    assert(AppOneWireParser_ProcessTimeout(
        &parser, 106U) == true);
    assert(parser.receiving == false);
}

static void test_invalid_build_arguments(void)
{
    uint8_t output[APP_ONEWIRE_MAX_FRAME_LEN];
    uint8_t data = 0U;

    assert(AppOneWire_BuildFrame(
        1U, 2U, NULL, 1U, output, sizeof(output)) == 0U);
    assert(AppOneWire_BuildFrame(
        1U, 2U, &data, APP_ONEWIRE_MAX_DATA_LEN + 1U,
        output, sizeof(output)) == 0U);
    assert(AppOneWire_BuildFrame(
        1U, 2U, &data, 1U, output, 5U) == 0U);
}

int main(void)
{
    test_build_known_frames();
    test_parse_known_frame();
    test_xor_error();
    test_format_error_and_recovery();
    test_interbyte_timeout();
    test_invalid_build_arguments();

    puts("app_onewire_protocol tests passed");
    return 0;
}
