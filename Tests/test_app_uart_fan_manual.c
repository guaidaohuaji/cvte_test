#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_auto_control.h"
#include "app_damper.h"
#include "app_fan.h"
#include "app_fan_health.h"
#include "app_led.h"
#include "app_manual_fan_control.h"
#include "app_ntc.h"
#include "app_onewire.h"
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
static uint8_t tx_capture[80];
static uint16_t tx_capture_length;

static AppFanSnapshot fake_fan;
static bool fake_fan_snapshot_ok;
static AppManualFanControlSnapshot fake_manual;
static bool fake_manual_snapshot_ok;
static AppFanHealthSnapshot fake_health;
static bool fake_health_snapshot_ok;
static bool fake_health_clear_ok;
static uint32_t health_clear_calls;
static uint8_t fake_auto_mode;
static AppManualFanResult fake_manual_result;
static uint32_t off_calls;
static uint32_t duty_calls;
static uint32_t speed_calls;
static uint16_t last_manual_value;
static uint32_t pwm_config_calls;
static bool pwm_last_enabled;
static uint32_t pwm_last_frequency;
static uint16_t pwm_last_duty;
static bool pwm_config_result;

uint32_t HAL_GetTick(void) { return fake_tick; }

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

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (state == GPIO_PIN_SET)
        port->ODR |= pin;
    else
        port->ODR &= ~(uint32_t)pin;
}

/* Unrelated application objects required by app_uart.c. */
void AppLed_Init(void) { }
void AppLed_Process(void) { }
void AppLed_SetAutomatic(void) { }
void AppLed_SetManual(bool on) { (void)on; }
AppLedMode AppLed_GetMode(void) { return APP_LED_MODE_AUTO; }
bool AppLed_IsOn(void) { return false; }

bool AppPwm_Init(void) { return true; }
bool AppPwm_Configure(bool enabled, uint32_t frequency_hz, uint16_t duty_x100)
{
    pwm_config_calls++;
    pwm_last_enabled = enabled;
    pwm_last_frequency = frequency_hz;
    pwm_last_duty = duty_x100;
    return pwm_config_result;
}
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
    if (!fake_fan_snapshot_ok)
        return false;
    *snapshot = fake_fan;
    return true;
}

void AppFanHealth_Init(void) { }
void AppFanHealth_Process(void) { }
bool AppFanHealth_GetSnapshot(AppFanHealthSnapshot *snapshot)
{
    if (!fake_health_snapshot_ok)
        return false;
    *snapshot = fake_health;
    return true;
}
bool AppFanHealth_ClearFault(void)
{
    health_clear_calls++;
    return fake_health_clear_ok;
}
bool AppFanHealth_AuthorizeRestart(void) { return true; }
bool AppFanHealth_IsFaultLatched(void) { return fake_health.fault_latched != 0U; }
bool AppFanHealth_RestartIsInhibited(void)
{
    return fake_health.restart_inhibited != 0U;
}

void AppOneWire_Init(void) { }
void AppOneWire_Process(void) { }
AppOneWireRole AppOneWire_GetRole(void) { return APP_ONEWIRE_ROLE_VALUE_MASTER; }
AppOneWireLinkState AppOneWire_GetLinkState(void) { return APP_ONEWIRE_LINK_OFFLINE; }
uint8_t AppOneWire_GetLocalSlaveAddress(void)
{
    return APP_ONEWIRE_SLAVE_ADDRESS;
}
AppOneWireSubmitResult AppOneWire_Submit(uint8_t op, uint16_t address, uint16_t value)
{
    return AppOneWire_SubmitTo(
        APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS, op, address, value);
}
AppOneWireSubmitResult AppOneWire_SubmitTo(
    uint8_t slave_address, uint8_t op, uint16_t address, uint16_t value)
{
    (void)slave_address; (void)op; (void)address; (void)value;
    return APP_ONEWIRE_SUBMIT_OK;
}
void AppOneWire_GetSnapshot(AppOneWireSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->slave_address = APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS;
    snapshot->context_valid = true;
    snapshot->last_response_age_ms = 0xFFFFU;
}
bool AppOneWire_GetSnapshotForAddress(
    uint8_t slave_address, AppOneWireSnapshot *snapshot)
{
    AppOneWire_GetSnapshot(snapshot);
    snapshot->slave_address = slave_address;
    return snapshot->context_valid;
}

void AppDamper_Init(void) { }
void AppDamper_Process(void) { }
void AppDamper_TimerCallback(void) { }
void AppDamper_EmergencyShutdown(void) { }
bool AppDamper_GetSnapshot(DamperSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    return true;
}
uint8_t AppDamper_MoveAbsolute(int32_t target) { (void)target; return DAMPER_STATUS_OK; }
uint8_t AppDamper_MoveRelative(int32_t delta) { (void)delta; return DAMPER_STATUS_OK; }
uint8_t AppDamper_SetCurrentPosition(int32_t position) { (void)position; return DAMPER_STATUS_OK; }
uint8_t AppDamper_Stop(void) { return DAMPER_STATUS_OK; }
uint8_t AppDamper_Release(void) { return DAMPER_STATUS_OK; }

void AppAutoControl_Init(void) { }
void AppAutoControl_Process(void) { }
bool AppAutoControl_GetSnapshot(AppAutoControlSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->mode = fake_auto_mode;
    return true;
}
uint8_t AppAutoControl_SetMode(uint8_t mode)
{
    fake_auto_mode = mode;
    return APP_AUTO_SET_MODE_OK;
}
uint8_t AppAutoControl_GetMode(void) { return fake_auto_mode; }

void AppManualFanControl_Init(void) { }
void AppManualFanControl_Process(void) { }
AppManualFanResult AppManualFanControl_SetOff(void)
{
    off_calls++;
    return fake_manual_result;
}
AppManualFanResult AppManualFanControl_SetDuty(uint16_t duty_x100)
{
    duty_calls++;
    last_manual_value = duty_x100;
    return fake_manual_result;
}
AppManualFanResult AppManualFanControl_SetTargetRpm(uint16_t target_rpm)
{
    speed_calls++;
    last_manual_value = target_rpm;
    return fake_manual_result;
}
bool AppManualFanControl_GetSnapshot(AppManualFanControlSnapshot *snapshot)
{
    if (!fake_manual_snapshot_ok)
        return false;
    *snapshot = fake_manual;
    return true;
}

static uint8_t checksum(const uint8_t *frame, uint16_t length_without_checksum)
{
    uint8_t value = 0U;
    uint16_t i;
    for (i = 1U; i < length_without_checksum; i++)
        value = (uint8_t)(value + frame[i]);
    return value;
}

static uint16_t build_frame(uint8_t type, const uint8_t *data, uint8_t data_len,
                            uint8_t *frame)
{
    uint16_t total = (uint16_t)data_len + 4U;
    frame[0] = 0x7EU;
    frame[1] = type;
    frame[2] = (uint8_t)(data_len + 2U);
    memcpy(&frame[3], data, data_len);
    frame[3U + data_len] = checksum(frame, (uint16_t)(3U + data_len));
    return total;
}

static void feed_frame(const uint8_t *frame, uint16_t length)
{
    uint16_t i;
    tx_capture_length = 0U;
    for (i = 0U; i < length; i++)
    {
        assert(armed_rx_byte != NULL);
        *armed_rx_byte = frame[i];
        AppUart_RxCpltCallback();
    }
    AppUart_Process();
}

static void reset_state(void)
{
    memset(&fake_fan, 0, sizeof(fake_fan));
    memset(&fake_manual, 0, sizeof(fake_manual));
    memset(&fake_health, 0, sizeof(fake_health));
    fake_fan_snapshot_ok = true;
    fake_manual_snapshot_ok = true;
    fake_health_snapshot_ok = true;
    fake_health_clear_ok = true;
    health_clear_calls = 0U;
    fake_auto_mode = APP_AUTO_MODE_MANUAL;
    fake_manual_result = APP_MANUAL_FAN_RESULT_OK;
    off_calls = 0U;
    duty_calls = 0U;
    speed_calls = 0U;
    last_manual_value = 0U;
    pwm_config_calls = 0U;
    pwm_last_enabled = false;
    pwm_last_frequency = 0U;
    pwm_last_duty = 0U;
    pwm_config_result = true;
    fake_tick = 0U;
    tx_capture_length = 0U;
    test_gpioe.ODR = 0U;
    test_gpiod.ODR = 0U;
    AppUart_Init();
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static int16_t read_i16(const uint8_t *p)
{
    return (int16_t)read_u16(p);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static void assert_ok_response(uint8_t type)
{
    assert(tx_capture_length == 6U);
    assert(tx_capture[0] == 0x7EU);
    assert(tx_capture[1] == type);
    assert(tx_capture[2] == 0x04U);
    assert(tx_capture[3] == 0x00U);
    assert(tx_capture[4] == 0x06U);
    assert(tx_capture[5] == checksum(tx_capture, 5U));
}

static void test_legacy_query_is_unchanged(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U };
    uint16_t length;

    reset_state();
    fake_fan.state = APP_FAN_STATE_RUNNING;
    fake_fan.enabled = true;
    fake_fan.target_duty_x100 = 2345U;
    fake_fan.applied_duty_x100 = 2300U;
    fake_fan.pwm_frequency_hz = 10000U;
    fake_fan.fg_frequency_millihz = 50000U;
    fake_fan.rpm = 1500U;
    fake_fan.tach_age_ms = 321U;

    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(tx_capture_length == 22U);
    assert(tx_capture[1] == 0xA1U);
    assert(tx_capture[2] == 20U);
    assert(tx_capture[3] == 0U);
    assert(tx_capture[4] == 0x06U);
    assert(tx_capture[5] == APP_FAN_STATE_RUNNING);
    assert(tx_capture[6] == 1U);
    assert(read_u16(&tx_capture[7]) == 2345U);
    assert(read_u16(&tx_capture[9]) == 2300U);
    assert(read_u16(&tx_capture[11]) == 10000U);
    assert(read_u32(&tx_capture[13]) == 50000U);
    assert(read_u16(&tx_capture[17]) == 1500U);
    assert(read_u16(&tx_capture[19]) == 321U);
    assert(tx_capture[21] == checksum(tx_capture, 21U));
}

static void test_trailing_legacy_query_stays_legacy(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U, 0x7FU };
    uint16_t length;

    reset_state();
    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);
    assert(tx_capture_length == 22U);
    assert(tx_capture[2] == 20U);
}

static void test_extended_query_v1(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U, 0x01U };
    uint16_t length;
    const uint8_t *d;

    reset_state();
    fake_fan.state = APP_FAN_STATE_RUNNING;
    fake_fan.enabled = true;
    fake_fan.tach_valid = 1U;
    fake_fan.target_duty_x100 = 2400U;
    fake_fan.applied_duty_x100 = 2380U;
    fake_fan.pwm_frequency_hz = 10000U;
    fake_fan.fg_frequency_millihz = 50123U;
    fake_fan.rpm = 1504U;
    fake_fan.tach_age_ms = 44U;

    fake_manual.mode = APP_MANUAL_FAN_MODE_SPEED;
    fake_manual.control_state = APP_MANUAL_FAN_CTRL_IN_TOLERANCE;
    fake_manual.target_rpm = 1500U;
    fake_manual.rpm_error = -4;
    fake_manual.tach_valid = 1U;
    fake_manual.in_tolerance = 1U;
    fake_manual.feedforward_duty_x100 = 2333U;
    fake_manual.adjust_count = 70000U;
    fake_manual.fault_count = 7U;

    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(tx_capture_length == 36U);
    assert(tx_capture[1] == 0xA1U);
    assert(tx_capture[2] == 34U);
    d = &tx_capture[3];
    assert(d[0] == 0U && d[1] == 0x06U);
    assert(d[2] == 1U);
    assert(d[3] == APP_MANUAL_FAN_MODE_SPEED);
    assert(d[4] == APP_MANUAL_FAN_CTRL_IN_TOLERANCE);
    assert(d[5] == 0x0FU);
    assert(read_u16(&d[6]) == 1500U);
    assert(read_i16(&d[8]) == -4);
    assert(read_u16(&d[10]) == 2333U);
    assert(read_u16(&d[12]) == 2400U);
    assert(read_u16(&d[14]) == 2380U);
    assert(read_u16(&d[16]) == 10000U);
    assert(read_u32(&d[18]) == 50123U);
    assert(read_u16(&d[22]) == 1504U);
    assert(read_u16(&d[24]) == 44U);
    assert(read_u16(&d[26]) == 65535U);
    assert(read_u16(&d[28]) == 7U);
    assert(d[30] == APP_AUTO_MODE_MANUAL);
    assert(d[31] == 0U);
    assert(tx_capture[35] == checksum(tx_capture, 35U));
}

static void test_extended_v1_reports_safety_lock_as_fault(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U, 0x01U };
    uint16_t length;
    const uint8_t *d;

    reset_state();
    fake_fan.state = APP_FAN_STATE_SAFETY_LOCKED;
    fake_manual.control_state = APP_MANUAL_FAN_CTRL_SAFETY_LOCKED;

    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(tx_capture_length == 36U);
    d = &tx_capture[3];
    assert((d[5] & 0x80U) != 0U);
}

static void test_health_query_v2(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U, 0x02U };
    uint16_t length;
    const uint8_t *d;

    reset_state();
    fake_fan.state = APP_FAN_STATE_SAFETY_LOCKED;
    fake_fan.enabled = false;

    fake_health.state = APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED;
    fake_health.fault_type = APP_FAN_HEALTH_FAULT_SPEED_LOW;
    fake_health.fault_latched = 1U;
    fake_health.monitoring_active = 0U;
    fake_health.tach_valid = 1U;
    fake_health.shutdown_succeeded = 1U;
    fake_health.restart_inhibited = 1U;
    fake_health.applied_duty_x100 = 0U;
    fake_health.reference_duty_x100 = 3500U;
    fake_health.expected_rpm = 1800U;
    fake_health.actual_rpm = 1100U;
    fake_health.deviation_rpm = -700;
    fake_health.absolute_deviation_rpm = 700U;
    fake_health.maximum_absolute_deviation_rpm = 845U;
    fake_health.fault_applied_duty_x100 = 3500U;
    fake_health.fault_expected_rpm = 1800U;
    fake_health.fault_actual_rpm = 1100U;
    fake_health.fault_deviation_rpm = -700;
    fake_health.abnormal_elapsed_ms = 5000U;
    fake_health.settling_remaining_ms = 70000U;

    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(tx_capture_length == 36U);
    assert(tx_capture[1] == 0xA1U);
    assert(tx_capture[2] == 34U);
    d = &tx_capture[3];
    assert(d[0] == 0U && d[1] == 0x06U);
    assert(d[2] == 2U);
    assert(d[3] == APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED);
    assert(d[4] == APP_FAN_HEALTH_FAULT_SPEED_LOW);
    assert(d[5] == 0x5DU); /* latched, tach, shutdown, inhibited, safety */
    assert(read_u16(&d[6]) == 0U);
    assert(read_u16(&d[8]) == 3500U);
    assert(read_u16(&d[10]) == 1800U);
    assert(read_u16(&d[12]) == 1100U);
    assert(read_i16(&d[14]) == -700);
    assert(read_u16(&d[16]) == 700U);
    assert(read_u16(&d[18]) == 845U);
    assert(read_u16(&d[20]) == 3500U);
    assert(read_u16(&d[22]) == 1800U);
    assert(read_u16(&d[24]) == 1100U);
    assert(read_i16(&d[26]) == -700);
    assert(read_u16(&d[28]) == 5000U);
    assert(read_u16(&d[30]) == 65535U);
    assert(tx_capture[35] == checksum(tx_capture, 35U));
}

static void test_health_query_failure_maps_to_hw_error(void)
{
    uint8_t frame[8];
    const uint8_t data[] = { 0x06U, 0x02U };
    uint16_t length;

    reset_state();
    fake_health_snapshot_ok = false;
    length = build_frame(0xA1U, data, sizeof(data), frame);
    feed_frame(frame, length);
    assert(tx_capture_length == 6U);
    assert(tx_capture[3] == 0x0AU);
    assert(tx_capture[4] == 0x06U);
}

static void test_clear_fault_control_is_allowed_in_manual_and_auto(void)
{
    uint8_t frame[10];
    const uint8_t data[] = { 0x06U, 0x03U, 0x00U, 0x00U };
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, data, sizeof(data), frame);
    feed_frame(frame, length);
    assert(health_clear_calls == 1U);
    assert_ok_response(0xA2U);

    fake_auto_mode = APP_AUTO_MODE_AUTO;
    feed_frame(frame, length);
    assert(health_clear_calls == 2U);
    assert_ok_response(0xA2U);
}

static void test_clear_fault_validation_and_failure(void)
{
    uint8_t frame[12];
    const uint8_t nonzero[] = { 0x06U, 0x03U, 0x01U, 0x00U };
    const uint8_t trailing[] = { 0x06U, 0x03U, 0x00U, 0x00U, 0xAAU };
    const uint8_t valid[] = { 0x06U, 0x03U, 0x00U, 0x00U };
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, nonzero, sizeof(nonzero), frame);
    feed_frame(frame, length);
    assert(health_clear_calls == 0U);
    assert(tx_capture[3] == 0x05U);

    length = build_frame(0xA2U, trailing, sizeof(trailing), frame);
    feed_frame(frame, length);
    assert(health_clear_calls == 0U);
    assert(tx_capture[3] == 0x01U);

    fake_health_clear_ok = false;
    length = build_frame(0xA2U, valid, sizeof(valid), frame);
    feed_frame(frame, length);
    assert(health_clear_calls == 1U);
    assert(tx_capture[3] == 0x0AU);
}

static void test_legacy_pwm_control_routes_to_manual_controller(void)
{
    uint8_t frame[10];
    const uint8_t data[] = { 0x06U, 0x01U, 0xB8U, 0x0BU }; /* 3000 */
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(duty_calls == 1U);
    assert(last_manual_value == 3000U);
    assert_ok_response(0xA2U);
}

static void test_speed_control_routes_target_rpm(void)
{
    uint8_t frame[10];
    const uint8_t data[] = { 0x06U, 0x02U, 0xDCU, 0x05U }; /* 1500 */
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, data, sizeof(data), frame);
    feed_frame(frame, length);

    assert(speed_calls == 1U);
    assert(last_manual_value == 1500U);
    assert_ok_response(0xA2U);
}

static void test_off_requires_zero(void)
{
    uint8_t frame[10];
    const uint8_t invalid[] = { 0x06U, 0x00U, 0x01U, 0x00U };
    const uint8_t valid[] = { 0x06U, 0x00U, 0x00U, 0x00U };
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, invalid, sizeof(invalid), frame);
    feed_frame(frame, length);
    assert(off_calls == 0U);
    assert(tx_capture[3] == 0x05U);

    length = build_frame(0xA2U, valid, sizeof(valid), frame);
    feed_frame(frame, length);
    assert(off_calls == 1U);
    assert_ok_response(0xA2U);
}

static void test_result_mapping_and_length(void)
{
    uint8_t frame[10];
    const uint8_t speed[] = { 0x06U, 0x02U, 0xDCU, 0x05U };
    const uint8_t short_data[] = { 0x06U, 0x02U, 0xDCU };
    uint16_t length;

    reset_state();
    fake_manual_result = APP_MANUAL_FAN_RESULT_MODE_LOCKED;
    length = build_frame(0xA2U, speed, sizeof(speed), frame);
    feed_frame(frame, length);
    assert(tx_capture[3] == 0x0BU);

    fake_manual_result = APP_MANUAL_FAN_RESULT_INVALID_PARAM;
    feed_frame(frame, length);
    assert(tx_capture[3] == 0x05U);

    fake_manual_result = APP_MANUAL_FAN_RESULT_HW_ERROR;
    feed_frame(frame, length);
    assert(tx_capture[3] == 0x0AU);

    length = build_frame(0xA2U, short_data, sizeof(short_data), frame);
    feed_frame(frame, length);
    assert(tx_capture[3] == 0x01U);
}


static void test_global_auto_locks_manual_commands_first(void)
{
    uint8_t frame[10];
    const uint8_t malformed[] = { 0x06U, 0x02U };
    uint16_t length;

    reset_state();
    fake_auto_mode = APP_AUTO_MODE_AUTO;
    length = build_frame(0xA2U, malformed, sizeof(malformed), frame);
    feed_frame(frame, length);

    assert(off_calls == 0U);
    assert(duty_calls == 0U);
    assert(speed_calls == 0U);
    assert(tx_capture[3] == 0x0BU);
}

static void test_control_trailing_bytes_remain_compatible(void)
{
    uint8_t frame[12];
    const uint8_t data[] = { 0x06U, 0x02U, 0xDCU, 0x05U, 0xAAU };
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, data, sizeof(data), frame);
    feed_frame(frame, length);
    assert(speed_calls == 1U);
    assert(last_manual_value == 1500U);
    assert_ok_response(0xA2U);
}


static void test_general_pwm_atomic_control(void)
{
    uint8_t frame[16];
    const uint8_t enable_data[] = {
        0x01U, 0x01U, 0xA0U, 0x86U, 0x01U, 0x00U, 0x64U, 0x00U
    }; /* 100000 Hz, 1.00% */
    const uint8_t disable_data[] = {
        0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, enable_data, sizeof(enable_data), frame);
    feed_frame(frame, length);
    assert(pwm_config_calls == 1U);
    assert(pwm_last_enabled);
    assert(pwm_last_frequency == 100000U);
    assert(pwm_last_duty == 100U);
    assert(tx_capture[3] == 0x00U && tx_capture[4] == 0x01U);

    length = build_frame(0xA2U, disable_data, sizeof(disable_data), frame);
    feed_frame(frame, length);
    assert(pwm_config_calls == 2U);
    assert(!pwm_last_enabled);
    assert(pwm_last_frequency == 0U);
    assert(pwm_last_duty == 0U);
}

static void test_general_pwm_strict_length_and_range(void)
{
    uint8_t frame[20];
    const uint8_t trailing[] = {
        0x01U, 0x01U, 0xA0U, 0x86U, 0x01U, 0x00U, 0x64U, 0x00U, 0xAAU
    };
    const uint8_t too_high[] = {
        0x01U, 0x01U, 0x41U, 0xD1U, 0x0CU, 0x00U, 0x64U, 0x00U
    }; /* 840001 Hz */
    uint16_t length;

    reset_state();
    length = build_frame(0xA2U, trailing, sizeof(trailing), frame);
    feed_frame(frame, length);
    assert(pwm_config_calls == 0U);
    assert(tx_capture[3] == 0x01U);

    length = build_frame(0xA2U, too_high, sizeof(too_high), frame);
    feed_frame(frame, length);
    assert(pwm_config_calls == 0U);
    assert(tx_capture[3] == 0x05U);
}

int main(void)
{
    test_legacy_query_is_unchanged();
    test_trailing_legacy_query_stays_legacy();
    test_extended_query_v1();
    test_extended_v1_reports_safety_lock_as_fault();
    test_health_query_v2();
    test_health_query_failure_maps_to_hw_error();
    test_clear_fault_control_is_allowed_in_manual_and_auto();
    test_clear_fault_validation_and_failure();
    test_legacy_pwm_control_routes_to_manual_controller();
    test_speed_control_routes_target_rpm();
    test_off_requires_zero();
    test_result_mapping_and_length();
    test_global_auto_locks_manual_commands_first();
    test_control_trailing_bytes_remain_compatible();
    test_general_pwm_atomic_control();
    test_general_pwm_strict_length_and_range();

    puts("app_uart fan control and health protocol tests passed");
    return 0;
}
