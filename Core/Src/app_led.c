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

static void apply_output(bool on)
{
    led_on = on;
    HAL_GPIO_WritePin(
        LED_GPIO_Port,
        LED_Pin,
        on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

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

void AppLed_Init(void)
{
    led_mode = APP_LED_MODE_AUTO;
    manual_on = false;
    enter_auto_profile(determine_auto_profile(), HAL_GetTick());
}

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

void AppLed_SetAutomatic(void)
{
    led_mode = APP_LED_MODE_AUTO;
    enter_auto_profile(determine_auto_profile(), HAL_GetTick());
}

void AppLed_SetManual(bool on)
{
    led_mode = APP_LED_MODE_MANUAL;
    manual_on = on;
    apply_output(on);
}

AppLedMode AppLed_GetMode(void)
{
    return led_mode;
}

bool AppLed_IsOn(void)
{
    return led_on;
}
