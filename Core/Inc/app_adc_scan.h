#ifndef APP_ADC_SCAN_H
#define APP_ADC_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define ADC_BUF_SIZE 1024U

bool AppAdcScan_Init(void);
void AppAdcScan_Process(void);
bool AppAdcScan_GetNtcAverage(uint16_t *avg, uint32_t *samples);
bool AppAdcScan_GetCh0Block(const uint16_t **data, uint32_t *count, uint32_t *seq);

#endif
