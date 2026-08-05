/**
 * @file app_led.c
 * @brief PE8 状态指示灯（实现文件）。
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

#include "app_led.h"

#include <stdint.h>

#include "app_onewire.h"
#include "main.h"

#define APP_LED_MASTER_BLINK_MS 1000U
#define APP_LED_SLAVE_ONLINE_BLINK_MS 500U
#define APP_LED_SLAVE_FAULT_BLINK_MS 200U

typedef enum
{
    APP_LED_AUTO_PROFILE_MASTER = 0,
    APP_LED_AUTO_PROFILE_SLAVE_OFF,
    APP_LED_AUTO_PROFILE_SLAVE_ONLINE,
    APP_LED_AUTO_PROFILE_SLAVE_FAULT
} AppLedAutoProfile;

static AppLedMode led_mode;
static bool led_on;
static bool manual_on;
static AppLedAutoProfile auto_profile;
static uint32_t phase_start_tick;

/**
 * @brief 把已计算的目标值应用到硬件或下层模块。
 * @param on 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void apply_output(bool on)
{
    led_on = on;
    HAL_GPIO_WritePin(
        LED_GPIO_Port,
        LED_Pin,
        on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppLedAutoProfile determine_auto_profile(void)
{
    AppOneWireLinkState link_state;

    if (AppOneWire_GetRole() == APP_ONEWIRE_ROLE_VALUE_MASTER)
    {
        return APP_LED_AUTO_PROFILE_MASTER;
    }

    link_state = AppOneWire_GetLinkState();
    if (link_state == APP_ONEWIRE_LINK_ONLINE)
    {
        return APP_LED_AUTO_PROFILE_SLAVE_ONLINE;
    }
    if ((link_state == APP_ONEWIRE_LINK_STALE) ||
        (link_state == APP_ONEWIRE_LINK_UART_ERROR))
    {
        return APP_LED_AUTO_PROFILE_SLAVE_FAULT;
    }

    return APP_LED_AUTO_PROFILE_SLAVE_OFF;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t profile_interval_ms(AppLedAutoProfile profile)
{
    if (profile == APP_LED_AUTO_PROFILE_MASTER)
    {
        return APP_LED_MASTER_BLINK_MS;
    }
    if (profile == APP_LED_AUTO_PROFILE_SLAVE_ONLINE)
    {
        return APP_LED_SLAVE_ONLINE_BLINK_MS;
    }
    if (profile == APP_LED_AUTO_PROFILE_SLAVE_FAULT)
    {
        return APP_LED_SLAVE_FAULT_BLINK_MS;
    }

    return 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param profile 输入捕获自动量程档位。
 * @param now 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void enter_auto_profile(AppLedAutoProfile profile, uint32_t now)
{
    auto_profile = profile;
    phase_start_tick = now;

    if (profile == APP_LED_AUTO_PROFILE_SLAVE_OFF)
    {
        apply_output(false);
    }
    else
    {
        apply_output(true);
    }
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppLed_Init(void)
{
    led_mode = APP_LED_MODE_AUTO;
    manual_on = false;
    enter_auto_profile(determine_auto_profile(), HAL_GetTick());
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppLed_Process(void)
{
    AppLedAutoProfile requested_profile;
    uint32_t interval;
    uint32_t now;

    if (led_mode == APP_LED_MODE_MANUAL)
    {
        apply_output(manual_on);
        return;
    }

    now = HAL_GetTick();
    requested_profile = determine_auto_profile();
    if (requested_profile != auto_profile)
    {
        enter_auto_profile(requested_profile, now);
        return;
    }

    interval = profile_interval_ms(auto_profile);
    if (interval == 0U)
    {
        apply_output(false);
        phase_start_tick = now;
        return;
    }

    if ((uint32_t)(now - phase_start_tick) >= interval)
    {
        phase_start_tick = now;
        apply_output(!led_on);
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 */
void AppLed_SetAutomatic(void)
{
    led_mode = APP_LED_MODE_AUTO;
    enter_auto_profile(determine_auto_profile(), HAL_GetTick());
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param on 见调用点；该参数只在本次调用期间有效。
 */
void AppLed_SetManual(bool on)
{
    led_mode = APP_LED_MODE_MANUAL;
    manual_on = on;
    apply_output(on);
}

/**
 * @brief 返回当前工作模式。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
AppLedMode AppLed_GetMode(void)
{
    return led_mode;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppLed_IsOn(void)
{
    return led_on;
}
