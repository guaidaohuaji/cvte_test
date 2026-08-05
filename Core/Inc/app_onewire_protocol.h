/**
 * @file app_onewire_protocol.h
 * @brief 单总线帧格式与逐字节解析器（公共接口头文件）。
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


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
