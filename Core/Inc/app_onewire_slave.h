/**
 * @file app_onewire_slave.h
 * @brief 单总线从机握手、寄存器读写与 500 ms 失效（公共接口头文件）。
 *
 * 模块职责：仅响应目标地址等于本机地址的帧，完成两阶段握手后提供寄存器读写；500 ms 无本机有效帧则进入通信故障。
 * 数据输入：USART6 完整帧、发送完成事件和时间。
 * 数据输出：握手/读写响应；本机寄存器空间、LED 使用的从机状态和统计。
 * 执行上下文：其他从机地址的帧静默忽略，不刷新本机 500 ms 计时。
 * 阅读重点：按地址过滤→握手状态→ONLINE 读写→COMM_FAULT 超时阅读。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_ONEWIRE_SLAVE_H
#define APP_ONEWIRE_SLAVE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_1 = 0,
    APP_ONEWIRE_SLAVE_WAIT_HANDSHAKE_2,
    APP_ONEWIRE_SLAVE_ONLINE,
    APP_ONEWIRE_SLAVE_COMM_FAULT
} AppOneWireSlaveState;

typedef struct
{
    uint32_t parser_frame_count;
    uint32_t parser_xor_error_count;
    uint32_t parser_format_error_count;
    uint32_t parser_timeout_count;
    uint32_t handshake_1_count;
    uint32_t handshake_2_count;
    uint32_t handshake_timeout_count;
    uint32_t write_count;
    uint32_t read_count;
    uint32_t invalid_frame_count;
    uint32_t ignored_foreign_frame_count;
    uint32_t invalid_address_count;
    uint32_t invalid_operation_count;
    uint32_t response_queued_count;
    uint32_t response_sent_count;
    uint32_t response_send_fail_count;
    uint32_t response_busy_drop_count;
    uint32_t comm_fault_count;
} AppOneWireSlaveStats;

void AppOneWireSlave_Init(void);
void AppOneWireSlave_Process(void);

AppOneWireSlaveState AppOneWireSlave_GetState(void);
bool AppOneWireSlave_IsResponsePending(void);
bool AppOneWireSlave_ReadRegister(uint16_t address, uint16_t *value);
void AppOneWireSlave_GetStats(AppOneWireSlaveStats *stats);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
