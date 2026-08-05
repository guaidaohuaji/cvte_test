/**
 * @file app_led.h
 * @brief PE8 状态指示灯（公共接口头文件）。
 *
 * 模块职责：支持手动亮灭和根据单总线角色/链路状态自动选择闪烁节奏。
 * 数据输入：手动命令；AppOneWire 链路状态和角色。
 * 数据输出：PE8 低有效 LED 电平。
 * 执行上下文：主循环按 HAL tick 非阻塞翻转，不使用延时。
 * 阅读重点：看 determine_auto_profile() 如何把通信状态映射成闪烁配置。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#ifndef APP_LED_H
#define APP_LED_H

#include <stdbool.h>

typedef enum
{
    APP_LED_MODE_AUTO = 0,
    APP_LED_MODE_MANUAL = 1
} AppLedMode;

void AppLed_Init(void);
void AppLed_Process(void);

void AppLed_SetAutomatic(void);
void AppLed_SetManual(bool on);

AppLedMode AppLed_GetMode(void);
bool AppLed_IsOn(void);


/*
 * 学习提示：
 * 1. 先读配置宏、枚举和结构体，确认单位、范围与状态语义；
 * 2. 若存在 Snapshot，区分目标值、实际值、有效标志和诊断计数；
 * 3. 最后读 API，区分命令接口、周期 Process、HAL 回调和只读查询。
 */
#endif
