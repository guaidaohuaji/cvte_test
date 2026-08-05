/**
 * @file app_onewire.h
 * @brief 单总线角色无关门面层（公共接口头文件）。
 *
 * 模块职责：根据编译期角色把统一 API 转发到 Master 或 Slave 实现，使 W2、LED 和 main.c 不需要散布角色判断。
 * 数据输入：编译宏 APP_ONEWIRE_ROLE；上层提交和查询。
 * 数据输出：统一角色、链路、提交和快照 API。
 * 执行上下文：Master 构建包含多从机事务；Slave 构建只运行本机从机状态机。
 * 阅读重点：这是理解编译期角色隔离的入口，先读此文件再分别进入 master/slave。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_ONEWIRE_H
#define APP_ONEWIRE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_ONEWIRE_ROLE_VALUE_MASTER = 1,
    APP_ONEWIRE_ROLE_VALUE_SLAVE = 2
} AppOneWireRole;

typedef enum
{
    APP_ONEWIRE_LINK_OFFLINE = 0,
    APP_ONEWIRE_LINK_HANDSHAKING = 1,
    APP_ONEWIRE_LINK_ONLINE = 2,
    APP_ONEWIRE_LINK_STALE = 3,
    APP_ONEWIRE_LINK_UART_ERROR = 4
} AppOneWireLinkState;

typedef enum
{
    APP_ONEWIRE_RESULT_SUCCESS = 0x00,
    APP_ONEWIRE_RESULT_PENDING = 0x01,
    APP_ONEWIRE_RESULT_RESPONSE_TIMEOUT = 0x08,
    APP_ONEWIRE_RESULT_RESPONSE_ERROR = 0x09,
    APP_ONEWIRE_RESULT_HANDSHAKE_FAILED = 0x0A,
    APP_ONEWIRE_RESULT_UART_ERROR = 0x0B
} AppOneWireResultCode;

typedef enum
{
    APP_ONEWIRE_SUBMIT_OK = 0,
    APP_ONEWIRE_SUBMIT_BUSY,
    APP_ONEWIRE_SUBMIT_INVALID_OPERATION,
    APP_ONEWIRE_SUBMIT_INVALID_ADDRESS,
    APP_ONEWIRE_SUBMIT_NOT_MASTER,
    APP_ONEWIRE_SUBMIT_INVALID_SLAVE,
    APP_ONEWIRE_SUBMIT_NO_CONTEXT
} AppOneWireSubmitResult;

typedef struct
{
    AppOneWireRole role;
    AppOneWireLinkState link_state;
    bool busy;
    bool pending_valid;
    uint8_t last_operation;
    AppOneWireResultCode result_code;
    uint16_t address;
    uint16_t value;
    uint8_t slave_address;
    bool context_valid;
    uint16_t last_response_age_ms;
} AppOneWireSnapshot;

void AppOneWire_Init(void);
void AppOneWire_Process(void);

AppOneWireRole AppOneWire_GetRole(void);
AppOneWireLinkState AppOneWire_GetLinkState(void);
uint8_t AppOneWire_GetLocalSlaveAddress(void);

/* Backward-compatible default target: 0x02. */
AppOneWireSubmitResult AppOneWire_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value);

AppOneWireSubmitResult AppOneWire_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value);

void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot);
bool AppOneWire_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireSnapshot *snapshot);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
