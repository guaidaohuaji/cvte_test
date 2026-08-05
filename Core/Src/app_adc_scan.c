/**
 * @file app_adc_scan.c
 * @brief ADC1 双通道 DMA 统一采集层（实现文件）。
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

#include "app_adc_scan.h"
#include "adc.h"
#include "tim.h"
#include <stddef.h>

/* ADC1 规则组有两个 Rank，DMA 缓冲按 [CH0, CH9, CH0, CH9, ...]
 * 交错排列。处理代码必须保持与 adc.c 中 Rank 顺序一致。 */
static uint16_t dma_buf[ADC_BUF_SIZE];
static volatile uint8_t half_ready;
static volatile uint8_t full_ready;
static volatile uint32_t overrun_count;
static volatile uint32_t dma_seq;
static uint32_t last_seq;

static uint64_t ntc_sum;
static uint32_t ntc_count;

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppAdcScan_Init(void)
{
    half_ready    = 0U;
    full_ready    = 0U;
    overrun_count = 0U;
    dma_seq       = 0U;
    last_seq      = 0U;
    ntc_sum       = 0ULL;
    ntc_count     = 0U;

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)dma_buf, ADC_BUF_SIZE) != HAL_OK)
        return false;

    if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
    {
        HAL_ADC_Stop_DMA(&hadc1);
        return false;
    }

    return true;
}

/**
 * @brief 消费 DMA 半缓冲/全缓冲就绪标志，把交错双通道数据累加到 NTC，并发布风机 CH0 数据块。
 */
void AppAdcScan_Process(void)
{
    uint32_t seq = dma_seq;
    if (seq == last_seq) return;

    if (half_ready)
    {
        half_ready = 0U;
        for (uint32_t i = 0U; i < ADC_BUF_SIZE / 2U; i += 2U)
        {
            ntc_sum   += dma_buf[i + 1U];
            ntc_count += 1U;
        }
        last_seq = seq;
    }

    if (full_ready)
    {
        full_ready = 0U;
        for (uint32_t i = ADC_BUF_SIZE / 2U; i < ADC_BUF_SIZE; i += 2U)
        {
            ntc_sum   += dma_buf[i + 1U];
            ntc_count += 1U;
        }
        last_seq = seq;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param avg 见调用点；该参数只在本次调用期间有效。
 * @param samples 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppAdcScan_GetNtcAverage(uint16_t *avg, uint32_t *samples)
{
    if (avg == NULL || samples == NULL) return false;
    if (ntc_count == 0U) return false;
    *avg     = (uint16_t)((ntc_sum + ntc_count / 2U) / ntc_count);
    *samples = ntc_count;
    ntc_sum   = 0ULL;
    ntc_count = 0U;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param data 输入数据缓冲区。
 * @param count 见调用点；该参数只在本次调用期间有效。
 * @param seq 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppAdcScan_GetCh0Block(const uint16_t **data, uint32_t *count, uint32_t *seq)
{
    if (data == NULL || count == NULL || seq == NULL) return false;
    *data  = dma_buf;
    *count = ADC_BUF_SIZE;
    *seq   = dma_seq;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
uint32_t AppAdcScan_GetOverrunCount(void)
{
    return overrun_count;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param hadc 见调用点；该参数只在本次调用期间有效。
 */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    if (half_ready) overrun_count++;
    half_ready = 1U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param hadc 见调用点；该参数只在本次调用期间有效。
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    if (full_ready) overrun_count++;
    full_ready = 1U;
    dma_seq++;
}
