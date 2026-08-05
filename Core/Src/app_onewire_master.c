/**
 * @file app_onewire_master.c
 * @brief 单总线一主多从按需握手状态机（实现文件）。
 *
 * 模块职责：维护最多 8 个从机上下文，在收到一次读写请求后判断目标链路新鲜度，必要时自动两次握手并继续原操作。
 * 数据输入：W2 提交的目标从机/操作/地址/值；USART6 完整响应帧和错误事件。
 * 数据输出：握手/读写单总线帧；每从机状态快照和统计。
 * 执行上下文：只有一个物理事务槽，任意时刻仅访问一个从机；不后台扫描、不保活。
 * 阅读重点：先看 contexts[] 与 pending_*，再看 SubmitTo()，之后沿 begin_handshake()→handle_*_response()→finish_* 阅读。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_onewire_master.h"

#include <stddef.h>
#include <string.h>

#include "app_onewire_config.h"
#include "app_onewire_protocol.h"
#include "app_onewire_uart.h"
#include "stm32f4xx_hal.h"

#define INVALID_CONTEXT_INDEX 0xFFU

static const uint8_t handshake_data_1[4] = {
    0x01U, 0x02U, 0x03U, 0x04U
};

static const uint8_t handshake_data_2[4] = {
    0x04U, 0x03U, 0x02U, 0x01U
};

typedef struct
{
    bool in_use;
    uint8_t slave_address;
    AppOneWireLinkState link_state;
    AppOneWireResultCode result_code;
    uint8_t last_operation;
    uint16_t address;
    uint16_t value;
    uint32_t last_valid_rx_tick;
    bool last_valid_rx_tick_valid;
} AppOneWireSlaveContext;

static AppOneWireParser parser;
static AppOneWireMasterState master_state;
static AppOneWireMasterStats master_stats;
/* 每个地址独立保存链路新鲜度和最后结果；但 USART6 是共享物理总线，
 * busy_flag 和 active_context_index 仍然是全局唯一事务槽。 */
static AppOneWireSlaveContext contexts[APP_ONEWIRE_MASTER_MAX_SLAVES];

static uint8_t tx_frame[APP_ONEWIRE_MAX_FRAME_LEN];
static uint8_t active_context_index;
static bool busy_flag;
/* pending_* 保存用户最初提交的读写。若链路过期，状态机会先握手，
 * 握手成功后继续执行这些字段，而不要求上位机再次发送命令。 */
static bool pending_valid;
static uint8_t pending_operation;
static uint16_t pending_address;
static uint16_t pending_value;

static uint32_t handshake_start_tick;
static uint32_t tx_start_tick;
static uint32_t response_wait_start_tick;
static uint32_t observed_uart_error_count;

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param slave_address 目标从机地址。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool slave_address_is_valid(uint8_t slave_address)
{
    return (slave_address >= APP_ONEWIRE_SLAVE_ADDR_MIN) &&
           (slave_address <= APP_ONEWIRE_SLAVE_ADDR_MAX);
}

/**
 * @brief 按数组索引取得已分配的从机上下文。
 * @param index contexts[] 数组索引。
 * @return 有效上下文指针；索引越界或槽位未使用时返回 NULL。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppOneWireSlaveContext *context_at(uint8_t index)
{
    if ((index >= APP_ONEWIRE_MASTER_MAX_SLAVES) ||
        !contexts[index].in_use)
    {
        return NULL;
    }
    return &contexts[index];
}

/**
 * @brief 取得当前占用物理总线事务槽的从机上下文。
 * @return 当前上下文指针；总线空闲或索引无效时返回 NULL。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppOneWireSlaveContext *active_context(void)
{
    return context_at(active_context_index);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param slave_address 目标从机地址。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint8_t find_context_index(uint8_t slave_address)
{
    uint8_t index;

    for (index = 0U; index < APP_ONEWIRE_MASTER_MAX_SLAVES; ++index)
    {
        if (contexts[index].in_use &&
            (contexts[index].slave_address == slave_address))
        {
            return index;
        }
    }

    return INVALID_CONTEXT_INDEX;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param context 目标从机上下文。
 * @param slave_address 目标从机地址。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void initialize_context(
    AppOneWireSlaveContext *context,
    uint8_t slave_address)
{
    *context = (AppOneWireSlaveContext){0};
    context->in_use = true;
    context->slave_address = slave_address;
    context->link_state = APP_ONEWIRE_LINK_OFFLINE;
    context->result_code = APP_ONEWIRE_RESULT_SUCCESS;
}

/**
 * @brief 返回或查找内部状态。
 * @param slave_address 目标从机地址。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint8_t get_or_create_context_index(uint8_t slave_address)
{
    uint8_t index = find_context_index(slave_address);

    if (index != INVALID_CONTEXT_INDEX)
    {
        return index;
    }

    for (index = 0U; index < APP_ONEWIRE_MASTER_MAX_SLAVES; ++index)
    {
        if (!contexts[index].in_use)
        {
            initialize_context(&contexts[index], slave_address);
            return index;
        }
    }

    master_stats.context_allocation_fail_count++;
    return INVALID_CONTEXT_INDEX;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param context 目标从机上下文。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t response_age_ms(
    const AppOneWireSlaveContext *context,
    uint32_t now_tick)
{
    uint32_t age;

    if ((context == NULL) || !context->last_valid_rx_tick_valid)
    {
        return 0xFFFFU;
    }

    age = (uint32_t)(now_tick - context->last_valid_rx_tick);
    return (age > 0xFFFEU) ? 0xFFFEU : (uint16_t)age;
}

/**
 * @brief 按当前时间更新某个从机的链路新鲜度；超过 450 ms 后只把主机缓存标为 STALE，不主动发送保活。
 * @param context 目标从机上下文。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void refresh_context_freshness(
    AppOneWireSlaveContext *context,
    uint32_t now_tick)
{
    if ((context != NULL) &&
        (context->link_state == APP_ONEWIRE_LINK_ONLINE) &&
        (!context->last_valid_rx_tick_valid ||
         ((uint32_t)(now_tick - context->last_valid_rx_tick) >=
          APP_ONEWIRE_MASTER_LINK_VALID_MS)))
    {
        context->link_state = APP_ONEWIRE_LINK_STALE;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void refresh_all_contexts(uint32_t now_tick)
{
    uint8_t index;

    for (index = 0U; index < APP_ONEWIRE_MASTER_MAX_SLAVES; ++index)
    {
        if (contexts[index].in_use)
        {
            refresh_context_freshness(&contexts[index], now_tick);
        }
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param context 目标从机上下文。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool link_is_fresh(
    AppOneWireSlaveContext *context,
    uint32_t now_tick)
{
    refresh_context_freshness(context, now_tick);
    return (context != NULL) &&
           (context->link_state == APP_ONEWIRE_LINK_ONLINE) &&
           context->last_valid_rx_tick_valid;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool guard_has_elapsed(uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    return (context == NULL) ||
           !context->last_valid_rx_tick_valid ||
           ((uint32_t)(now_tick - context->last_valid_rx_tick) >=
            APP_ONEWIRE_MASTER_GUARD_MS);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param context 目标从机上下文。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void mark_valid_response(
    AppOneWireSlaveContext *context,
    uint32_t now_tick)
{
    if (context == NULL)
    {
        return;
    }

    context->last_valid_rx_tick = now_tick;
    context->last_valid_rx_tick_valid = true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param actual 见调用点；该参数只在本次调用期间有效。
 * @param expected 见调用点；该参数只在本次调用期间有效。
 * @param length 数据长度。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 检查或处理单总线协议帧。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool frame_is_from_active_slave(const AppOneWireFrame *frame)
{
    AppOneWireSlaveContext *context = active_context();

    return (context != NULL) &&
           (frame->source == context->slave_address) &&
           (frame->destination == APP_ONEWIRE_LOCAL_ADDR_MASTER);
}

/**
 * @brief 检查或处理单总线协议帧。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param data 输入数据缓冲区。
 * @param length 数据长度。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool frame_has_data(
    const AppOneWireFrame *frame,
    const uint8_t *data,
    uint8_t length)
{
    return (frame->length == length) &&
           data_matches(frame->data, data, length);
}

/**
 * @brief 清除内部临时状态，使后续流程重新同步。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void reset_pending(void)
{
    pending_valid = false;
    pending_operation = 0U;
    pending_address = 0U;
    pending_value = 0U;
}

/**
 * @brief 完成当前阶段并清理事务状态。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void finish_transaction(void)
{
    reset_pending();
    busy_flag = false;
    active_context_index = INVALID_CONTEXT_INDEX;
    master_state = APP_ONEWIRE_MASTER_IDLE;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void drain_rx_and_reset_parser(void)
{
    uint8_t byte;

    while (AppOneWireUart_ReadByte(&byte))
    {
        /* Discard stale bytes before starting a new transaction. */
    }
    AppOneWireParser_Reset(&parser);
}

/**
 * @brief 完成当前阶段并清理事务状态。
 * @param value 写入值，读取操作时通常为 0。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void finish_success(uint16_t value, uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    if (context != NULL)
    {
        context->value = value;
        context->result_code = APP_ONEWIRE_RESULT_SUCCESS;
        context->link_state = APP_ONEWIRE_LINK_ONLINE;
        mark_valid_response(context, now_tick);
    }

    finish_transaction();
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param timed_out 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fail_handshake(bool timed_out)
{
    AppOneWireSlaveContext *context = active_context();

    if (timed_out)
    {
        master_stats.handshake_timeout_count++;
    }
    if (context != NULL)
    {
        context->result_code = APP_ONEWIRE_RESULT_HANDSHAKE_FAILED;
        context->link_state = APP_ONEWIRE_LINK_STALE;
    }
    finish_transaction();
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param code 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fail_response(AppOneWireResultCode code)
{
    AppOneWireSlaveContext *context = active_context();

    if (code == APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT)
    {
        master_stats.operation_timeout_count++;
    }
    else if (code == APP_ONEWIRE_RESULT_RESPONSE_ERROR)
    {
        master_stats.response_error_count++;
    }

    if (context != NULL)
    {
        context->result_code = code;
        context->link_state = APP_ONEWIRE_LINK_STALE;
    }
    finish_transaction();
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fail_uart(void)
{
    AppOneWireSlaveContext *context = active_context();

    master_stats.uart_error_count++;
    if (context != NULL)
    {
        context->result_code = APP_ONEWIRE_RESULT_UART_ERROR;
        context->link_state = APP_ONEWIRE_LINK_UART_ERROR;
    }
    finish_transaction();
}

/**
 * @brief 启动异步硬件动作或状态机阶段。
 * @param data 输入数据缓冲区。
 * @param data_length 见调用点；该参数只在本次调用期间有效。
 * @param tx_state 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool start_frame_tx(
    const uint8_t *data,
    uint8_t data_length,
    AppOneWireMasterState tx_state,
    uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();
    uint8_t frame_length;

    if (context == NULL)
    {
        fail_uart();
        return false;
    }

    frame_length = AppOneWire_BuildFrame(
        APP_ONEWIRE_LOCAL_ADDR_MASTER,
        context->slave_address,
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

/**
 * @brief 为当前目标从机启动两阶段握手，并保留原读写命令，使握手成功后能自动继续用户最初提交的操作。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void begin_handshake(uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    if (context == NULL)
    {
        fail_uart();
        return;
    }

    drain_rx_and_reset_parser();
    context->link_state = APP_ONEWIRE_LINK_HANDSHAKING;
    handshake_start_tick = now_tick;
    master_stats.handshake_attempt_count++;

    (void)start_frame_tx(
        handshake_data_1,
        (uint8_t)sizeof(handshake_data_1),
        APP_ONEWIRE_MASTER_HANDSHAKE_1_TX,
        now_tick);
}

/**
 * @brief 启动异步硬件动作或状态机阶段。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void start_pending_operation(uint32_t now_tick)
{
    uint8_t data[5];

    if (!pending_valid)
    {
        finish_transaction();
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

/**
 * @brief 在第二次握手成功后把目标从机标记 ONLINE；若有待执行读写则进入保护间隔，否则结束显式重握手事务。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void complete_handshake(uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    master_stats.handshake_2_ok_count++;
    if (context != NULL)
    {
        context->link_state = APP_ONEWIRE_LINK_ONLINE;
        mark_valid_response(context, now_tick);
    }

    if (pending_valid &&
        (pending_operation == APP_ONEWIRE_OPERATION_REHANDSHAKE))
    {
        if (context != NULL)
        {
            context->result_code = APP_ONEWIRE_RESULT_SUCCESS;
        }
        finish_transaction();
    }
    else if (pending_valid)
    {
        master_state = APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION;
    }
    else
    {
        if (context != NULL)
        {
            context->result_code = APP_ONEWIRE_RESULT_SUCCESS;
        }
        finish_transaction();
    }
}

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void handle_handshake_1_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    if (!frame_is_from_active_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(context, now_tick);

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

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void handle_handshake_2_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();

    if (!frame_is_from_active_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(context, now_tick);

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

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void handle_write_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();
    uint8_t expected[5];

    if (!frame_is_from_active_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(context, now_tick);

    expected[0] = APP_ONEWIRE_OPERATION_WRITE;
    expected[1] = (uint8_t)(pending_address >> 8U);
    expected[2] = (uint8_t)pending_address;
    expected[3] = (uint8_t)(pending_value >> 8U);
    expected[4] = (uint8_t)pending_value;

    if (!frame_has_data(frame, expected, (uint8_t)sizeof(expected)))
    {
        fail_response(APP_ONEWIRE_RESULT_RESPONSE_ERROR);
        return;
    }

    master_stats.write_ok_count++;
    finish_success(pending_value, now_tick);
}

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void handle_read_response(
    const AppOneWireFrame *frame,
    uint32_t now_tick)
{
    AppOneWireSlaveContext *context = active_context();
    uint16_t value;

    if (!frame_is_from_active_slave(frame))
    {
        master_stats.unexpected_frame_count++;
        return;
    }

    mark_valid_response(context, now_tick);

    if ((frame->length != 5U) ||
        (frame->data[0] != APP_ONEWIRE_OPERATION_READ) ||
        (frame->data[1] != (uint8_t)(pending_address >> 8U)) ||
        (frame->data[2] != (uint8_t)pending_address))
    {
        fail_response(APP_ONEWIRE_RESULT_RESPONSE_ERROR);
        return;
    }

    value = (uint16_t)(((uint16_t)frame->data[3] << 8U) |
                       frame->data[4]);
    master_stats.read_ok_count++;
    finish_success(value, now_tick);
}

/**
 * @brief 根据当前主状态解释完整响应帧；状态与帧类型必须匹配，否则记为意外响应而不误推进事务。
 * @param frame 见调用点；该参数只在本次调用期间有效。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool state_is_handshaking(void)
{
    return (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_TX) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT) ||
           (master_state == APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_TX) ||
           (master_state == APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT);
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param now_tick 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_handshake_total_timeout(uint32_t now_tick)
{
    if (state_is_handshaking() &&
        ((uint32_t)(now_tick - handshake_start_tick) >=
         APP_ONEWIRE_HANDSHAKE_TOTAL_MS))
    {
        fail_handshake(true);
    }
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppOneWireMaster_Init(void)
{
    AppOneWireUartStats uart_stats;

    AppOneWireParser_Init(&parser);
    (void)memset(&master_stats, 0, sizeof(master_stats));
    (void)memset(contexts, 0, sizeof(contexts));
    initialize_context(&contexts[0], APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS);

    master_state = APP_ONEWIRE_MASTER_IDLE;
    active_context_index = INVALID_CONTEXT_INDEX;
    busy_flag = false;
    reset_pending();

    handshake_start_tick = 0U;
    tx_start_tick = 0U;
    response_wait_start_tick = 0U;

    AppOneWireUart_GetStats(&uart_stats);
    observed_uart_error_count = uart_stats.uart_error_count;
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppOneWireMaster_Process(void)
{
    uint32_t now_tick = HAL_GetTick();

    if (AppOneWireParser_ProcessTimeout(&parser, now_tick))
    {
        master_stats.parser_timeout_count++;
    }

    refresh_all_contexts(now_tick);
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

    process_wait_timeout(now_tick);
    process_handshake_total_timeout(now_tick);
}

/**
 * @brief 向默认目标提交异步事务。
 * @param operation 协议操作码。
 * @param address 寄存器地址。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireSubmitResult AppOneWireMaster_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    return AppOneWireMaster_SubmitTo(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS,
        operation,
        address,
        value);
}

/**
 * @brief 提交一个面向指定从机的事务。若总线空闲则建立/查找上下文并保存 pending 命令；是否先握手由 Process() 根据链路年龄决定。
 * @param slave_address 目标从机地址。
 * @param operation 协议操作码。
 * @param address 寄存器地址。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireSubmitResult AppOneWireMaster_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    AppOneWireSlaveContext *context;
    uint8_t context_index;
    uint32_t now_tick;

    if (busy_flag)
    {
        return APP_ONEWIRE_SUBMIT_BUSY;
    }

    if (!slave_address_is_valid(slave_address))
    {
        return APP_ONEWIRE_SUBMIT_INVALID_SLAVE;
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

    context_index = get_or_create_context_index(slave_address);
    if (context_index == INVALID_CONTEXT_INDEX)
    {
        return APP_ONEWIRE_SUBMIT_NO_CONTEXT;
    }
    context = &contexts[context_index];

    if (operation == APP_ONEWIRE_OPERATION_REHANDSHAKE)
    {
        address = 0U;
        value = 0U;
    }

    pending_valid = true;
    pending_operation = operation;
    pending_address = address;
    pending_value = value;
    active_context_index = context_index;

    context->last_operation = operation;
    context->address = address;
    context->value = value;
    context->result_code = APP_ONEWIRE_RESULT_PENDING;
    busy_flag = true;

    now_tick = HAL_GetTick();
    if ((operation == APP_ONEWIRE_OPERATION_REHANDSHAKE) ||
        !link_is_fresh(context, now_tick))
    {
        master_state = APP_ONEWIRE_MASTER_HANDSHAKE_START;
    }
    else
    {
        master_state = APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION;
    }

    return APP_ONEWIRE_SUBMIT_OK;
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param slave_address 目标从机地址。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppOneWireMaster_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireMasterSnapshot *snapshot)
{
    AppOneWireSlaveContext *context;
    uint8_t index;
    uint32_t now_tick;

    if (snapshot == NULL)
    {
        return false;
    }

    *snapshot = (AppOneWireMasterSnapshot){0};
    snapshot->slave_address = slave_address;
    snapshot->master_state = busy_flag ? master_state : APP_ONEWIRE_MASTER_IDLE;
    snapshot->link_state = APP_ONEWIRE_LINK_OFFLINE;
    snapshot->busy = busy_flag;
    snapshot->result_code = APP_ONEWIRE_RESULT_SUCCESS;
    snapshot->last_response_age_ms = 0xFFFFU;

    if (!slave_address_is_valid(slave_address))
    {
        return false;
    }

    index = find_context_index(slave_address);
    if (index == INVALID_CONTEXT_INDEX)
    {
        return false;
    }

    context = &contexts[index];
    now_tick = HAL_GetTick();
    refresh_context_freshness(context, now_tick);

    snapshot->context_valid = true;
    snapshot->link_state = context->link_state;
    snapshot->master_state = busy_flag
        ? master_state
        : APP_ONEWIRE_MASTER_IDLE;
    /* busy is the shared physical-bus state. pending_valid is scoped to the
     * selected slave context. */
    snapshot->busy = busy_flag;
    snapshot->pending_valid = busy_flag &&
                              (active_context_index == index) &&
                              pending_valid;
    snapshot->last_operation = context->last_operation;
    snapshot->result_code = context->result_code;
    snapshot->address = context->address;
    snapshot->value = context->value;
    snapshot->last_response_age_ms = response_age_ms(context, now_tick);
    return true;
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 */
void AppOneWireMaster_GetSnapshot(AppOneWireMasterSnapshot *snapshot)
{
    (void)AppOneWireMaster_GetSnapshotForAddress(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS,
        snapshot);
}

/**
 * @brief 复制内部诊断统计。
 * @param stats 输出统计结构指针。
 */
void AppOneWireMaster_GetStats(AppOneWireMasterStats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    *stats = master_stats;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireMasterState AppOneWireMaster_GetState(void)
{
    return master_state;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppOneWireLinkState AppOneWireMaster_GetLinkState(void)
{
    AppOneWireMasterSnapshot snapshot;

    (void)AppOneWireMaster_GetSnapshotForAddress(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS,
        &snapshot);
    return snapshot.link_state;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppOneWireMaster_IsBusy(void)
{
    return busy_flag;
}
