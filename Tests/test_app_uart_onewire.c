#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_auto_control.h"
#include "app_damper.h"
#include "app_fan.h"
#include "app_fan_health.h"
#include "app_manual_fan_control.h"
#include "app_led.h"
#include "app_ntc.h"
#include "app_onewire.h"
#include "app_onewire_config.h"
#include "app_pwm.h"
#include "app_pwm_input.h"
#include "app_uart.h"
#include "usart.h"

GPIO_TypeDef test_gpioe;
GPIO_TypeDef test_gpiod;
UART_HandleTypeDef huart1 = { USART1 };
UART_HandleTypeDef huart6 = { USART6 };
static uint32_t fake_tick;
static uint8_t *armed_rx_byte;
static uint8_t tx_capture[64];
static uint16_t tx_capture_length;
static AppOneWireSnapshot fake_snapshot;
static AppOneWireSubmitResult fake_submit_result;
static uint8_t submitted_slave_address;
static uint8_t submitted_operation;
static uint16_t submitted_address;
static uint16_t submitted_value;
static uint32_t submit_call_count;
static AppLedMode fake_led_mode;
static bool fake_led_on;
static uint32_t led_auto_call_count;
static uint32_t led_manual_call_count;

uint32_t HAL_GetTick(void)
{
    return fake_tick;
}

HAL_StatusTypeDef HAL_UART_Transmit(
    UART_HandleTypeDef *huart,
    uint8_t *data,
    uint16_t length,
    uint32_t timeout)
{
    (void)timeout;
    assert(huart == &huart1);
    assert(length <= sizeof(tx_capture));
    memcpy(tx_capture, data, length);
    tx_capture_length = length;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive_IT(
    UART_HandleTypeDef *huart,
    uint8_t *data,
    uint16_t length)
{
    assert(huart == &huart1);
    assert(length == 1U);
    armed_rx_byte = data;
    return HAL_OK;
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

void AppLed_Init(void) { }
void AppLed_Process(void) { }
void AppLed_SetAutomatic(void)
{
    fake_led_mode = APP_LED_MODE_AUTO;
    led_auto_call_count++;
}
void AppLed_SetManual(bool on)
{
    fake_led_mode = APP_LED_MODE_MANUAL;
    fake_led_on = on;
    led_manual_call_count++;
}
AppLedMode AppLed_GetMode(void) { return fake_led_mode; }
bool AppLed_IsOn(void) { return fake_led_on; }

bool AppPwm_Init(void) { return true; }
bool AppPwm_Configure(bool enabled, uint32_t frequency_hz, uint16_t duty_x100)
{ (void)enabled; (void)frequency_hz; (void)duty_x100; return true; }
bool AppPwm_SetFrequency(uint32_t value) { (void)value; return true; }
bool AppPwm_SetDutyX100(uint16_t value) { (void)value; return true; }
bool AppPwm_Enable(bool value) { (void)value; return true; }
bool AppPwm_IsEnabled(void) { return false; }
uint32_t AppPwm_GetTargetFrequency(void) { return 0U; }
uint32_t AppPwm_GetActualFrequency(void) { return 0U; }
int32_t AppPwm_GetFrequencyErrorPpm(void) { return 0; }
uint16_t AppPwm_GetTargetDutyX100(void) { return 0U; }
uint16_t AppPwm_GetActualDutyX100(void) { return 0U; }
uint32_t AppPwm_GetPrescaler(void) { return 0U; }
uint32_t AppPwm_GetAutoReload(void) { return 0U; }
uint32_t AppPwm_GetCompare(void) { return 0U; }

bool AppPwmInput_Init(void) { return true; }
void AppPwmInput_Process(void) { }
void AppPwmInput_CC_Callback(TIM_HandleTypeDef *htim) { (void)htim; }
void AppPwmInput_UP_Callback(TIM_HandleTypeDef *htim) { (void)htim; }
bool AppPwmInput_GetSnapshot(AppPwmInputSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

bool AppNtc_Init(void) { return true; }
void AppNtc_Process(void) { }
bool AppNtc_GetSnapshot(AppNtcSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

bool AppFan_Init(void) { return true; }
void AppFan_Process(void) { }
bool AppFan_SetEnabled(bool enabled, uint16_t duty)
{
    (void)enabled;
    (void)duty;
    return true;
}
bool AppFan_GetSnapshot(AppFanSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}

void AppDamper_Init(void) { }
void AppDamper_Process(void) { }
void AppDamper_TimerCallback(void) { }
void AppDamper_EmergencyShutdown(void) { }
bool AppDamper_GetSnapshot(DamperSnapshot *snapshot)
{ memset(snapshot, 0, sizeof(*snapshot)); return true; }
uint8_t AppDamper_MoveAbsolute(int32_t target)
{ (void)target; return DAMPER_STATUS_OK; }
uint8_t AppDamper_MoveRelative(int32_t delta)
{ (void)delta; return DAMPER_STATUS_OK; }
uint8_t AppDamper_SetCurrentPosition(int32_t position)
{ (void)position; return DAMPER_STATUS_OK; }
uint8_t AppDamper_Stop(void) { return DAMPER_STATUS_OK; }
uint8_t AppDamper_Release(void) { return DAMPER_STATUS_OK; }

void AppAutoControl_Init(void) { }
void AppAutoControl_Process(void) { }
bool AppAutoControl_GetSnapshot(AppAutoControlSnapshot *snapshot)
{ memset(snapshot, 0, sizeof(*snapshot)); return true; }
uint8_t AppAutoControl_SetMode(uint8_t mode)
{ (void)mode; return APP_AUTO_SET_MODE_OK; }
uint8_t AppAutoControl_GetMode(void) { return APP_AUTO_MODE_MANUAL; }

void AppManualFanControl_Init(void) { }
void AppManualFanControl_Process(void) { }
AppManualFanResult AppManualFanControl_SetOff(void)
{ return APP_MANUAL_FAN_RESULT_OK; }
AppManualFanResult AppManualFanControl_SetDuty(uint16_t duty_x100)
{ (void)duty_x100; return APP_MANUAL_FAN_RESULT_OK; }
AppManualFanResult AppManualFanControl_SetTargetRpm(uint16_t target_rpm)
{ (void)target_rpm; return APP_MANUAL_FAN_RESULT_OK; }
bool AppManualFanControl_GetSnapshot(AppManualFanControlSnapshot *snapshot)
{ memset(snapshot, 0, sizeof(*snapshot)); return true; }

void AppFanHealth_Init(void) { }
void AppFanHealth_Process(void) { }
bool AppFanHealth_GetSnapshot(AppFanHealthSnapshot *snapshot)
{ memset(snapshot, 0, sizeof(*snapshot)); return true; }
bool AppFanHealth_ClearFault(void) { return true; }
bool AppFanHealth_AuthorizeRestart(void) { return true; }
bool AppFanHealth_IsFaultLatched(void) { return false; }
bool AppFanHealth_RestartIsInhibited(void) { return false; }

void AppOneWire_Init(void) { }
void AppOneWire_Process(void) { }
AppOneWireRole AppOneWire_GetRole(void) { return fake_snapshot.role; }
AppOneWireLinkState AppOneWire_GetLinkState(void)
{
    return fake_snapshot.link_state;
}
uint8_t AppOneWire_GetLocalSlaveAddress(void)
{
    return APP_ONEWIRE_SLAVE_ADDRESS;
}
AppOneWireSubmitResult AppOneWire_Submit(
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    return AppOneWire_SubmitTo(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS, operation, address, value);
}
AppOneWireSubmitResult AppOneWire_SubmitTo(
    uint8_t slave_address,
    uint8_t operation,
    uint16_t address,
    uint16_t value)
{
    submitted_slave_address = slave_address;
    submitted_operation = operation;
    submitted_address = address;
    submitted_value = value;
    submit_call_count++;
    return fake_submit_result;
}
void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot)
{
    *snapshot = fake_snapshot;
}
bool AppOneWire_GetSnapshotForAddress(
    uint8_t slave_address,
    AppOneWireSnapshot *snapshot)
{
    *snapshot = fake_snapshot;
    snapshot->slave_address = slave_address;
    return snapshot->context_valid;
}

static uint8_t w2_checksum(const uint8_t *frame, uint16_t length_without_checksum)
{
    uint8_t checksum = 0U;
    uint16_t index;

    for (index = 1U; index < length_without_checksum; index++)
    {
        checksum = (uint8_t)(checksum + frame[index]);
    }
    return checksum;
}

static void feed_frame(const uint8_t *frame, uint16_t length)
{
    uint16_t index;

    tx_capture_length = 0U;
    for (index = 0U; index < length; index++)
    {
        assert(armed_rx_byte != NULL);
        *armed_rx_byte = frame[index];
        AppUart_RxCpltCallback();
    }
    AppUart_Process();
}

static void reset_test_state(void)
{
    memset(&fake_snapshot, 0, sizeof(fake_snapshot));
    fake_submit_result = APP_ONEWIRE_SUBMIT_OK;
    submitted_slave_address = 0U;
    submitted_operation = 0U;
    submitted_address = 0U;
    submitted_value = 0U;
    submit_call_count = 0U;
    tx_capture_length = 0U;
    fake_tick = 0U;
    fake_led_mode = APP_LED_MODE_AUTO;
    fake_led_on = false;
    led_auto_call_count = 0U;
    led_manual_call_count = 0U;
    test_gpioe.ODR = 0U;
    test_gpiod.ODR = 0U;
    AppUart_Init();
}

static void test_query_snapshot(void)
{
    const uint8_t query[] = { 0x7EU, 0xA1U, 0x03U, 0x08U, 0xACU };
    const uint8_t expected_data[] = {
        0x00U, 0x08U, 0x01U, 0x02U,
        0x01U, 0x01U, 0x03U, 0x01U,
        0x08U, 0x00U, 0x34U, 0x12U,
        0x02U, 0x01U, 0x0CU, 0x00U
    };

    reset_test_state();
    fake_snapshot.role = APP_ONEWIRE_ROLE_VALUE_MASTER;
    fake_snapshot.link_state = APP_ONEWIRE_LINK_ONLINE;
    fake_snapshot.busy = true;
    fake_snapshot.pending_valid = true;
    fake_snapshot.last_operation = APP_ONEWIRE_OPERATION_WRITE;
    fake_snapshot.result_code = APP_ONEWIRE_RESULT_PENDING;
    fake_snapshot.address = 0x0008U;
    fake_snapshot.value = 0x1234U;
    fake_snapshot.context_valid = true;
    fake_snapshot.last_response_age_ms = 12U;

    feed_frame(query, (uint16_t)sizeof(query));

    assert(tx_capture_length == 20U);
    assert(tx_capture[0] == 0x7EU);
    assert(tx_capture[1] == 0xA1U);
    assert(tx_capture[2] == 0x12U);
    assert(memcmp(&tx_capture[3], expected_data, sizeof(expected_data)) == 0);
    assert(tx_capture[19] == w2_checksum(tx_capture, 19U));
}

static void test_write_submit(void)
{
    const uint8_t command[] = {
        0x7EU, 0xA2U, 0x08U, 0x08U, 0x03U,
        0x08U, 0x00U, 0x34U, 0x12U, 0x03U
    };
    const uint8_t expected[] = { 0x7EU, 0xA2U, 0x04U, 0x00U, 0x08U, 0xAEU };

    reset_test_state();
    feed_frame(command, (uint16_t)sizeof(command));

    assert(submit_call_count == 1U);
    assert(submitted_slave_address == APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS);
    assert(submitted_operation == APP_ONEWIRE_OPERATION_WRITE);
    assert(submitted_address == 0x0008U);
    assert(submitted_value == 0x1234U);
    assert(tx_capture_length == sizeof(expected));
    assert(memcmp(tx_capture, expected, sizeof(expected)) == 0);
}

static void test_read_submit(void)
{
    const uint8_t command[] = {
        0x7EU, 0xA2U, 0x08U, 0x08U, 0x06U,
        0x08U, 0x00U, 0x00U, 0x00U, 0xC0U
    };

    reset_test_state();
    feed_frame(command, (uint16_t)sizeof(command));

    assert(submit_call_count == 1U);
    assert(submitted_slave_address == APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS);
    assert(submitted_operation == APP_ONEWIRE_OPERATION_READ);
    assert(submitted_address == 0x0008U);
    assert(submitted_value == 0U);
    assert(tx_capture[3] == 0x00U);
    assert(tx_capture[4] == 0x08U);
}

static void test_targeted_query_and_control(void)
{
    uint8_t query[] = {0x7EU, 0xA1U, 0x04U, 0x08U, 0x03U, 0x00U};
    uint8_t command[] = {
        0x7EU, 0xA2U, 0x09U, 0x08U, 0x03U, 0x06U,
        0x09U, 0x00U, 0x00U, 0x00U, 0x00U
    };

    query[5] = w2_checksum(query, 5U);
    command[10] = w2_checksum(command, 10U);

    reset_test_state();
    fake_snapshot.role = APP_ONEWIRE_ROLE_VALUE_MASTER;
    fake_snapshot.link_state = APP_ONEWIRE_LINK_STALE;
    fake_snapshot.context_valid = true;
    fake_snapshot.last_response_age_ms = 0x01F4U;
    feed_frame(query, (uint16_t)sizeof(query));

    assert(tx_capture_length == 20U);
    assert(tx_capture[15] == 0x03U); /* DATA[12]: selected slave */
    assert(tx_capture[16] == 0x01U); /* DATA[13]: context valid */
    assert(tx_capture[17] == 0xF4U);
    assert(tx_capture[18] == 0x01U);

    feed_frame(command, (uint16_t)sizeof(command));
    assert(submit_call_count == 1U);
    assert(submitted_slave_address == 0x03U);
    assert(submitted_operation == APP_ONEWIRE_OPERATION_READ);
    assert(submitted_address == 0x0009U);
    assert(submitted_value == 0U);
}

static void test_busy_status(void)
{
    const uint8_t command[] = {
        0x7EU, 0xA2U, 0x08U, 0x08U, 0x03U,
        0x08U, 0x00U, 0x34U, 0x12U, 0x03U
    };
    const uint8_t expected[] = { 0x7EU, 0xA2U, 0x04U, 0x09U, 0x08U, 0xB7U };

    reset_test_state();
    fake_submit_result = APP_ONEWIRE_SUBMIT_BUSY;
    feed_frame(command, (uint16_t)sizeof(command));

    assert(tx_capture_length == sizeof(expected));
    assert(memcmp(tx_capture, expected, sizeof(expected)) == 0);
}

static void test_slave_rejects_control(void)
{
    const uint8_t command[] = {
        0x7EU, 0xA2U, 0x08U, 0x08U, 0x01U,
        0x00U, 0x00U, 0x00U, 0x00U, 0xB3U
    };
    const uint8_t expected[] = { 0x7EU, 0xA2U, 0x04U, 0x07U, 0x08U, 0xB5U };

    reset_test_state();
    fake_submit_result = APP_ONEWIRE_SUBMIT_NOT_MASTER;
    feed_frame(command, (uint16_t)sizeof(command));

    assert(submit_call_count == 1U);
    assert(tx_capture_length == sizeof(expected));
    assert(memcmp(tx_capture, expected, sizeof(expected)) == 0);
}

static void test_read_requires_zero_value(void)
{
    uint8_t command[] = {
        0x7EU, 0xA2U, 0x08U, 0x08U, 0x06U,
        0x08U, 0x00U, 0x01U, 0x00U, 0x00U
    };
    const uint8_t expected[] = { 0x7EU, 0xA2U, 0x04U, 0x05U, 0x08U, 0xB3U };

    command[9] = w2_checksum(command, 9U);
    reset_test_state();
    feed_frame(command, (uint16_t)sizeof(command));

    assert(submit_call_count == 0U);
    assert(tx_capture_length == sizeof(expected));
    assert(memcmp(tx_capture, expected, sizeof(expected)) == 0);
}

static void test_exact_control_length(void)
{
    const uint8_t command[] = {
        0x7EU, 0xA2U, 0x07U, 0x08U, 0x03U,
        0x08U, 0x00U, 0x34U, 0xF0U
    };
    const uint8_t expected[] = { 0x7EU, 0xA2U, 0x04U, 0x01U, 0x08U, 0xAFU };

    reset_test_state();
    feed_frame(command, (uint16_t)sizeof(command));

    assert(submit_call_count == 0U);
    assert(tx_capture_length == sizeof(expected));
    assert(memcmp(tx_capture, expected, sizeof(expected)) == 0);
}

static void test_led_query_uses_arbitrated_state(void)
{
    const uint8_t query[] = { 0x7EU, 0xA1U, 0x03U, 0x02U, 0xA6U };

    reset_test_state();
    fake_led_mode = APP_LED_MODE_MANUAL;
    fake_led_on = true;
    feed_frame(query, (uint16_t)sizeof(query));

    assert(tx_capture_length == 8U);
    assert(tx_capture[3] == 0x00U);
    assert(tx_capture[4] == 0x02U);
    assert(tx_capture[5] == (uint8_t)APP_LED_MODE_MANUAL);
    assert(tx_capture[6] == 0x01U);
    assert(tx_capture[7] == w2_checksum(tx_capture, 7U));
}

static void test_led_control_routes_through_arbiter(void)
{
    uint8_t command[] = { 0x7EU, 0xA2U, 0x04U, 0x02U, 0x01U, 0x00U };

    command[5] = w2_checksum(command, 5U);
    reset_test_state();
    feed_frame(command, (uint16_t)sizeof(command));
    assert(led_manual_call_count == 1U);
    assert(fake_led_mode == APP_LED_MODE_MANUAL);
    assert(fake_led_on);

    command[4] = 0x00U;
    command[5] = w2_checksum(command, 5U);
    feed_frame(command, (uint16_t)sizeof(command));
    assert(led_manual_call_count == 2U);
    assert(!fake_led_on);

    command[4] = 0x02U;
    command[5] = w2_checksum(command, 5U);
    feed_frame(command, (uint16_t)sizeof(command));
    assert(led_auto_call_count == 1U);
    assert(fake_led_mode == APP_LED_MODE_AUTO);
}

int main(void)
{
    test_query_snapshot();
    test_write_submit();
    test_read_submit();
    test_targeted_query_and_control();
    test_busy_status();
    test_slave_rejects_control();
    test_read_requires_zero_value();
    test_exact_control_length();
    test_led_query_uses_arbitrated_state();
    test_led_control_routes_through_arbiter();

    puts("app_uart one-wire object tests passed");
    return 0;
}
