#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_led.h"
#include "app_onewire.h"
#include "main.h"

GPIO_TypeDef test_gpioe;

static uint32_t fake_tick;
static AppOneWireRole fake_role;
static AppOneWireLinkState fake_link_state;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

void HAL_GPIO_WritePin(
    GPIO_TypeDef *port,
    uint16_t pin,
    GPIO_PinState state)
{
    if (state == GPIO_PIN_SET)
    {
        port->ODR |= pin;
    }
    else
    {
        port->ODR &= ~(uint32_t)pin;
    }
}

AppOneWireRole AppOneWire_GetRole(void)
{
    return fake_role;
}

AppOneWireLinkState AppOneWire_GetLinkState(void)
{
    return fake_link_state;
}

static void reset_as(AppOneWireRole role, AppOneWireLinkState link_state)
{
    fake_tick = 0U;
    fake_role = role;
    fake_link_state = link_state;
    test_gpioe.ODR = GPIO_PIN_8;
    AppLed_Init();
}

static void test_master_keeps_original_auto_blink(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_MASTER, APP_ONEWIRE_LINK_OFFLINE);

    assert(AppLed_GetMode() == APP_LED_MODE_AUTO);
    assert(AppLed_IsOn());
    assert((test_gpioe.ODR & GPIO_PIN_8) == 0U);

    fake_tick = 999U;
    AppLed_Process();
    assert(AppLed_IsOn());

    fake_tick = 1000U;
    AppLed_Process();
    assert(!AppLed_IsOn());
    assert((test_gpioe.ODR & GPIO_PIN_8) != 0U);

    fake_tick = 2000U;
    AppLed_Process();
    assert(AppLed_IsOn());
}

static void test_manual_mode_has_priority(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_SLAVE, APP_ONEWIRE_LINK_ONLINE);

    AppLed_SetManual(false);
    assert(AppLed_GetMode() == APP_LED_MODE_MANUAL);
    assert(!AppLed_IsOn());

    fake_link_state = APP_ONEWIRE_LINK_STALE;
    fake_tick = 1000U;
    AppLed_Process();
    assert(!AppLed_IsOn());

    AppLed_SetManual(true);
    assert(AppLed_IsOn());
    fake_tick = 5000U;
    AppLed_Process();
    assert(AppLed_IsOn());
}

static void test_slave_waiting_is_off(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_SLAVE, APP_ONEWIRE_LINK_OFFLINE);
    assert(!AppLed_IsOn());

    fake_link_state = APP_ONEWIRE_LINK_HANDSHAKING;
    AppLed_Process();
    assert(!AppLed_IsOn());
}

static void test_slave_online_blinks_500_ms(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_SLAVE, APP_ONEWIRE_LINK_OFFLINE);

    fake_tick = 10U;
    fake_link_state = APP_ONEWIRE_LINK_ONLINE;
    AppLed_Process();
    assert(AppLed_IsOn());

    fake_tick = 509U;
    AppLed_Process();
    assert(AppLed_IsOn());

    fake_tick = 510U;
    AppLed_Process();
    assert(!AppLed_IsOn());

    fake_tick = 1010U;
    AppLed_Process();
    assert(AppLed_IsOn());
}

static void test_slave_fault_blinks_200_ms(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_SLAVE, APP_ONEWIRE_LINK_ONLINE);

    fake_tick = 20U;
    fake_link_state = APP_ONEWIRE_LINK_STALE;
    AppLed_Process();
    assert(AppLed_IsOn());

    fake_tick = 219U;
    AppLed_Process();
    assert(AppLed_IsOn());

    fake_tick = 220U;
    AppLed_Process();
    assert(!AppLed_IsOn());

    fake_tick = 300U;
    fake_link_state = APP_ONEWIRE_LINK_UART_ERROR;
    AppLed_Process();
    assert(!AppLed_IsOn());

    fake_tick = 420U;
    AppLed_Process();
    assert(AppLed_IsOn());
}

static void test_return_to_auto_applies_current_profile_immediately(void)
{
    reset_as(APP_ONEWIRE_ROLE_VALUE_SLAVE, APP_ONEWIRE_LINK_OFFLINE);
    AppLed_SetManual(true);
    assert(AppLed_IsOn());

    AppLed_SetAutomatic();
    assert(AppLed_GetMode() == APP_LED_MODE_AUTO);
    assert(!AppLed_IsOn());

    fake_link_state = APP_ONEWIRE_LINK_ONLINE;
    AppLed_Process();
    assert(AppLed_IsOn());
}

int main(void)
{
    test_master_keeps_original_auto_blink();
    test_manual_mode_has_priority();
    test_slave_waiting_is_off();
    test_slave_online_blinks_500_ms();
    test_slave_fault_blinks_200_ms();
    test_return_to_auto_applies_current_profile_immediately();

    puts("app_led arbitration tests passed");
    return 0;
}
