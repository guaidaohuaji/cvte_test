/**
 * @file app_uart.h
 * @brief W2 上位机二进制协议入口（公共接口头文件）。
 *
 * 模块职责：接收 USART1 字节流，解析 7E/A1/A2 帧，按对象号分发查询或控制命令，并把各业务模块的快照编码成 W2 应答。
 * 数据输入：USART1 RX 中断写入的环形缓冲；各业务模块公开的查询接口。
 * 数据输出：USART1 二进制应答；对 PWM、LED、风机、风门、AUTO 和单总线模块的控制调用。
 * 执行上下文：RX 中断只收一个字节并重新使能接收；完整解析、参数检查和应答均在主循环 AppUart_Process() 中完成。
 * 阅读重点：先看对象号和状态码宏，再看 dispatch_frame()，最后按对象进入 process_*_query/control()。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_UART_H
#define APP_UART_H

#include "stm32f4xx_hal.h"

void AppUart_Init(void);
void AppUart_Process(void);
void AppUart_RxCpltCallback(void);
void AppUart_ErrorCallback(UART_HandleTypeDef *huart);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
