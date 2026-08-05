/**
 * @file app_onewire_protocol.c
 * @brief 单总线帧格式与逐字节解析器（实现文件）。
 *
 * 模块职责：实现 AA|S|D|L|DATA|XOR 帧的构造、异或校验、字节间超时和逐字节状态机解析。
 * 数据输入：USART6 接收字节及当前 tick。
 * 数据输出：完整 AppOneWireFrame 或解析错误/超时计数。
 * 执行上下文：协议层不关心主从业务，只负责帧边界和校验。
 * 阅读重点：按 WAIT_HEADER→SOURCE→DESTINATION→LENGTH→DATA→XOR 阅读。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_onewire_protocol.h"

#include <string.h>

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param parser 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void parser_start_frame(AppOneWireParser *parser, uint32_t now_tick)
{
    parser->buffer[0] = APP_ONEWIRE_FRAME_HEADER;
    parser->index = 1U;
    parser->expected_total = 0U;
    parser->running_xor = APP_ONEWIRE_FRAME_HEADER;
    parser->last_byte_tick = now_tick;
    parser->receiving = true;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @param parser 见调用点；该参数只在本次调用期间有效。
 */
void AppOneWireParser_Init(AppOneWireParser *parser)
{
    AppOneWireParser_Reset(parser);
}

/**
 * @brief 复位运行时状态和历史测量。
 * @param parser 见调用点；该参数只在本次调用期间有效。
 */
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

/**
 * @brief 执行一次非阻塞主循环处理。
 * @param parser 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param parser 见调用点；该参数只在本次调用期间有效。
 * @param byte 本次处理的接收字节。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @param frame_out 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param data 输入数据缓冲区。
 * @param length 数据长度。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param source 见调用点；该参数只在本次调用期间有效。
 * @param destination 见调用点；该参数只在本次调用期间有效。
 * @param data 输入数据缓冲区。
 * @param data_length 见调用点；该参数只在本次调用期间有效。
 * @param output 见调用点；该参数只在本次调用期间有效。
 * @param output_capacity 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
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
