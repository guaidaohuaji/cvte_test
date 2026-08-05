/**
 * @file app_onewire_uart.h
 * @brief USART6 单总线字节收发适配层（公共接口头文件）。
 *
 * 模块职责：管理单字节中断接收、256 字节环形缓冲、异步发送完成、UART 错误恢复和底层统计。
 * 数据输入：USART6 HAL RX/TX/Error 回调。
 * 数据输出：供协议状态机读取的字节流；非阻塞发送接口和统计。
 * 执行上下文：中断仅做最小搬运；主循环消费环形缓冲和处理重新挂接接收。
 * 阅读重点：重点看 RX re-arm、环形缓冲满处理以及 TX buffer 生命周期。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_ONEWIRE_UART_H
#define APP_ONEWIRE_UART_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t rx_byte_count;
    uint32_t rx_during_tx_count;
    uint32_t rx_overrun_count;
    uint32_t rx_rearm_fail_count;
    uint32_t uart_error_count;
    uint32_t tx_start_fail_count;
    uint32_t last_error_code;
    uint32_t tail_echo_dropped;
} AppOneWireUartStats;

void AppOneWireUart_Init(void);
void AppOneWireUart_Process(void);

bool AppOneWireUart_Send(const uint8_t *data, uint8_t length);
bool AppOneWireUart_IsTxBusy(void);
bool AppOneWireUart_TakeTxDone(void);

bool AppOneWireUart_RxAvailable(void);
bool AppOneWireUart_ReadByte(uint8_t *byte);

void AppOneWireUart_RxCpltCallback(void);
void AppOneWireUart_TxCpltCallback(void);
void AppOneWireUart_ErrorCallback(uint32_t error_code);

void AppOneWireUart_GetStats(AppOneWireUartStats *stats);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
