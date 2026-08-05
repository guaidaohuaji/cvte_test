/**
 * @file app_adc_scan.h
 * @brief ADC1 双通道 DMA 统一采集层（公共接口头文件）。
 *
 * 模块职责：统一拥有 ADC1 DMA 和 TIM2 触发资源，将交错的 PA0 风机反馈与 PB1 NTC 样本分发给上层。
 * 数据输入：ADC1 规则组 DMA 循环缓冲；半满和全满 HAL 回调。
 * 数据输出：NTC 累加平均；风机反馈块指针、长度与序号；DMA overrun 计数。
 * 执行上下文：HAL ADC 回调只设置就绪标志，真正的块处理在主循环执行，避免中断中做复杂计算。
 * 阅读重点：理解缓冲的偶数/奇数槽位对应 ADC Rank，及 half_ready/full_ready 与 dma_seq 的握手方式。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_ADC_SCAN_H
#define APP_ADC_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define ADC_BUF_SIZE 1024U

bool AppAdcScan_Init(void);
void AppAdcScan_Process(void);
bool AppAdcScan_GetNtcAverage(uint16_t *avg, uint32_t *samples);
bool AppAdcScan_GetCh0Block(const uint16_t **data, uint32_t *count, uint32_t *seq);
uint32_t AppAdcScan_GetOverrunCount(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
