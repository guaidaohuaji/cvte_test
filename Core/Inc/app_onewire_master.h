/**
 * @file app_onewire_master.h
 * @brief 单总线一主多从按需握手状态机（公共接口头文件）。
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

#ifndef APP_ONEWIRE_MASTER_H
#define APP_ONEWIRE_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_onewire.h"

typedef enum
{
    APP_ONEWIRE_MASTER_BOOT = 0,
    APP_ONEWIRE_MASTER_IDLE,
    APP_ONEWIRE_MASTER_HANDSHAKE_START,
    APP_ONEWIRE_MASTER_HANDSHAKE_1_TX,
    APP_ONEWIRE_MASTER_HANDSHAKE_1_WAIT,
    APP_ONEWIRE_MASTER_GUARD_BEFORE_HANDSHAKE_2,
    APP_ONEWIRE_MASTER_HANDSHAKE_2_TX,
    APP_ONEWIRE_MASTER_HANDSHAKE_2_WAIT,
    APP_ONEWIRE_MASTER_GUARD_BEFORE_OPERATION,
    APP_ONEWIRE_MASTER_ONLINE_IDLE,
    APP_ONEWIRE_MASTER_WRITE_TX,
    APP_ONEWIRE_MASTER_WRITE_WAIT,
    APP_ONEWIRE_MASTER_READ_TX,
    APP_ONEWIRE_MASTER_READ_WAIT,
    APP_ONEWIRE_MASTER_STALE_IDLE,
    APP_ONEWIRE_MASTER_ERROR_IDLE
} AppOneWireMasterState;

typedef struct
{
    AppOneWireLinkState link_state;
    AppOneWireMasterState master_state;
    bool busy;
    bool pending_valid;
    uint8_t last_operation;
    AppOneWireResultCode result_code;
    uint16_t address;
    uint16_t value;
    uint8_t slave_address;
    bool context_valid;
    uint16_t last_response_age_ms;
} AppOneWireMasterSnapshot;

typedef struct
{
    uint32_t parser_frame_count;
    uint32_t parser_xor_error_count;
    uint32_t parser_format_error_count;
    uint32_t parser_timeout_count;
    uint32_t unexpected_frame_count;
    uint32_t response_error_count;
    uint32_t handshake_attempt_count;
    uint32_t handshake_1_ok_count;
    uint32_t handshake_2_ok_count;
    uint32_t handshake_timeout_count;
    uint32_t write_tx_count;
    uint32_t write_ok_count;
    uint32_t read_tx_count;
    uint32_t read_ok_count;
    uint32_t operation_timeout_count;
    uint32_t tx_timeout_count;
    uint32_t uart_error_count;
    uint32_t context_allocation_fail_count;
} AppOneWireMasterStats;

void AppOneWireMaster_Init(void);
void AppOneWireMaster_Process(void);

AppOneWireSubmitResult AppOneWireMaster_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value);

AppOneWireSubmitResult AppOneWireMaster_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value);

void AppOneWireMaster_GetSnapshot(AppOneWireMasterSnapshot *snapshot);
bool AppOneWireMaster_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireMasterSnapshot *snapshot);
void AppOneWireMaster_GetStats(AppOneWireMasterStats *stats);

AppOneWireMasterState AppOneWireMaster_GetState(void);
AppOneWireLinkState AppOneWireMaster_GetLinkState(void);
bool AppOneWireMaster_IsBusy(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
