#include "app_adc_scan.h"
#include "adc.h"
#include "tim.h"
#include <stddef.h>

static uint16_t dma_buf[ADC_BUF_SIZE];
static volatile uint8_t half_ready;
static volatile uint8_t full_ready;
static volatile uint32_t overrun_count;
static volatile uint32_t dma_seq;
static uint32_t last_seq;

static uint64_t ntc_sum;
static uint32_t ntc_count;

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

bool AppAdcScan_GetCh0Block(const uint16_t **data, uint32_t *count, uint32_t *seq)
{
    if (data == NULL || count == NULL || seq == NULL) return false;
    *data  = dma_buf;
    *count = ADC_BUF_SIZE;
    *seq   = dma_seq;
    return true;
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    if (half_ready) overrun_count++;
    half_ready = 1U;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    if (full_ready) overrun_count++;
    full_ready = 1U;
    dma_seq++;
}
