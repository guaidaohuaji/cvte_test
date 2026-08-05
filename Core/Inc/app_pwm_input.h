/**
 * @file app_pwm_input.h
 * @brief TIM1 通用 PWM 输入测量（公共接口头文件）。
 *
 * 模块职责：默认使用 PWM Input + DMA 自动量程测量周期和外部高电平时间，同时保留 Step22 双边沿中断实现作为编译期回退。
 * 数据输入：PE9/TIM1 输入波形；TIM1 捕获寄存器和 DMA2 循环缓冲。
 * 数据输出：频率 mHz、占空比 x100、状态、量程和诊断计数快照。
 * 执行上下文：DMA 默认路径不使用捕获中断，AppPwmInput_Process() 在主循环批量读取最新样本；旧引擎通过宏选择。
 * 阅读重点：先看 app_pwm_input_config.h 的引擎与三档时钟，再看 DMA 每对数据的含义，最后看自动升降量程和静态电平超时。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_PWM_INPUT_H
#define APP_PWM_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

typedef enum {
    APP_PWM_IN_SEARCH = 0,
    APP_PWM_IN_OK,
    APP_PWM_IN_STATIC_HIGH,
    APP_PWM_IN_STATIC_LOW,
    APP_PWM_IN_OUT_OF_RANGE,
    APP_PWM_IN_UNSTABLE,
    APP_PWM_IN_HW_ERROR
} AppPwmInputStatus;

typedef enum {
    APP_PWM_INPUT_RANGE_FAST = 0,
    APP_PWM_INPUT_RANGE_MEDIUM = 1,
    APP_PWM_INPUT_RANGE_SLOW = 2
} AppPwmInputRangeProfile;

typedef struct {
    AppPwmInputStatus status;
    uint32_t freq_millihz;
    uint32_t duty_x100;
    uint32_t period_ticks;
    uint32_t ext_high_ticks;
    uint32_t age_ms;
    uint32_t ovc_count;
    uint16_t prescaler;
    uint8_t  synced;
    uint8_t  raw_pin_level;

    /* Internal diagnostics only. Object 0x03 does not transmit these fields. */
    uint8_t  engine_mode;
    uint8_t  range_profile;
    uint16_t dma_buffer_pairs;
    uint32_t dma_sample_count;
    uint32_t profile_switch_count;
    uint32_t dma_error_count;
} AppPwmInputSnapshot;

bool AppPwmInput_Init(void);
void AppPwmInput_Process(void);
void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim);
void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim);
bool AppPwmInput_GetSnapshot(AppPwmInputSnapshot *snap);

#if defined(APP_PWM_INPUT_HOST_TEST)
void AppPwmInput_TestFeedDmaPair(uint32_t period_ticks,
                                 uint32_t ext_high_ticks,
                                 uint32_t tick_ms);
void AppPwmInput_TestSignalOverflow(void);
void AppPwmInput_TestSignalDmaError(void);
uint8_t AppPwmInput_TestGetRangeProfile(void);
#endif


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
