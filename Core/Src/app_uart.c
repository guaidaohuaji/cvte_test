#include "app_uart.h"
#include "app_pwm.h"
#include "app_pwm_input.h"
#include "app_ntc.h"
#include "app_fan.h"
#include "app_onewire.h"
#include "app_onewire_config.h"
#include "app_led.h"
#include "app_damper.h"
#include "usart.h"
#include <stdbool.h>

#define RING_SIZE   128U
#define RING_MASK   (RING_SIZE - 1U)
#define FRAME_MAX_DATA  32U
#define FRAME_MAX_LEN   (FRAME_MAX_DATA + 2U)
#define FRAME_BUF_SIZE  36U
#define TIMEOUT_MS      50U

#define TYPE_QUERY   0xA1U
#define TYPE_CONTROL 0xA2U
#define FRAME_HEADER 0x7EU

#define OBJ_PWM_OUT  0x01U
#define OBJ_LED      0x02U
#define OBJ_PWM_IN   0x03U
#define OBJ_PD13     0x04U
#define OBJ_NTC      0x05U
#define OBJ_FAN      0x06U
#define OBJ_ONEWIRE  0x08U
#define OBJ_DAMPER   0x07U

#define DAMPER_A2_MOVE_ABSOLUTE        0x01U
#define DAMPER_A2_MOVE_RELATIVE        0x02U
#define DAMPER_A2_STOP                 0x03U
#define DAMPER_A2_RELEASE              0x04U
#define DAMPER_A2_SET_CURRENT_POSITION 0x05U

#define STATUS_OK                    0x00U
#define STATUS_LENGTH_ERROR          0x01U
#define STATUS_CHECKSUM_ERROR        0x02U
#define STATUS_UNSUPPORTED_TYPE      0x03U
#define STATUS_UNSUPPORTED_OBJECT    0x04U
#define STATUS_PARAM_RANGE           0x05U
#define STATUS_APPLY_FAILED          0x06U
#define STATUS_READ_ONLY             0x07U
#define STATUS_NO_VALID_DATA         0x08U
#define STATUS_BUSY                  0x09U
#define STATUS_HW_ERROR              0x0AU

typedef enum {
    PARSER_WAIT_HEADER = 0,
    PARSER_READ_TYPE,
    PARSER_READ_LENGTH,
    PARSER_READ_DATA,
    PARSER_READ_CHECKSUM
} ParserState;

static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static uint8_t          rx_ring[RING_SIZE];
static volatile uint8_t rx_byte;
static volatile bool    rx_overflow;

static uint8_t  frame_buf[FRAME_BUF_SIZE];
static uint8_t  frame_idx;
static uint8_t  frame_len;
static ParserState parser_state;
static uint32_t  last_byte_tick;

static bool ring_is_empty(void) { return rx_head == rx_tail; }
static bool ring_is_full(void)  { return ((rx_head + 1U) & RING_MASK) == rx_tail; }

static void ring_put(uint8_t byte)
{
    uint16_t next = (rx_head + 1U) & RING_MASK;
    rx_ring[rx_head] = byte;
    rx_head = next;
}

static uint8_t ring_get(void)
{
    uint8_t byte = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1U) & RING_MASK;
    return byte;
}

static void reset_parser(void)
{
    parser_state = PARSER_WAIT_HEADER;
    frame_idx    = 0U;
    frame_len    = 0U;
}

static void send_binary(const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 100U);
}

static bool SendW2Frame(uint8_t type, const uint8_t *data, uint8_t data_len)
{
    if (data_len > FRAME_MAX_DATA) return false;

    uint8_t buf[FRAME_BUF_SIZE];
    uint8_t total = 1U + 1U + 1U + data_len + 1U;

    buf[0] = FRAME_HEADER;
    buf[1] = type;
    buf[2] = data_len + 2U;

    uint8_t csum = buf[1] + buf[2];
    for (uint8_t i = 0U; i < data_len; i++)
    {
        buf[3U + i] = data[i];
        csum += data[i];
    }
    buf[3U + data_len] = csum;

    send_binary(buf, total);
    return true;
}

static void send_error(uint8_t type, uint8_t status, uint8_t obj_id)
{
    uint8_t d[2];
    d[0] = status;
    d[1] = obj_id;
    SendW2Frame(type, d, 2U);
}

static uint8_t onewire_submit_status(AppOneWireSubmitResult result)
{
    switch (result)
    {
    case APP_ONEWIRE_SUBMIT_OK:
        return STATUS_OK;
    case APP_ONEWIRE_SUBMIT_BUSY:
        return STATUS_BUSY;
    case APP_ONEWIRE_SUBMIT_INVALID_OPERATION:
    case APP_ONEWIRE_SUBMIT_INVALID_ADDRESS:
        return STATUS_PARAM_RANGE;
    case APP_ONEWIRE_SUBMIT_NOT_MASTER:
        return STATUS_READ_ONLY;
    default:
        return STATUS_APPLY_FAILED;
    }
}

static void process_onewire_query(void)
{
    AppOneWireSnapshot snapshot;
    uint8_t d[16];

    AppOneWire_GetSnapshot(&snapshot);

    d[0] = STATUS_OK;
    d[1] = OBJ_ONEWIRE;
    d[2] = (uint8_t)snapshot.role;
    d[3] = (uint8_t)snapshot.link_state;
    d[4] = snapshot.busy ? 1U : 0U;
    d[5] = snapshot.pending_valid ? 1U : 0U;
    d[6] = snapshot.last_operation;
    d[7] = (uint8_t)snapshot.result_code;
    d[8] = (uint8_t)snapshot.address;
    d[9] = (uint8_t)(snapshot.address >> 8U);
    d[10] = (uint8_t)snapshot.value;
    d[11] = (uint8_t)(snapshot.value >> 8U);
    d[12] = 0U;
    d[13] = 0U;
    d[14] = 0U;
    d[15] = 0U;

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

static void process_onewire_control(const uint8_t *data, uint8_t len)
{
    uint8_t operation;
    uint16_t address;
    uint16_t value;
    AppOneWireSubmitResult submit_result;
    uint8_t status;

    if (len != 6U)
    {
        send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_ONEWIRE);
        return;
    }

    operation = data[1];
    address = (uint16_t)data[2] | ((uint16_t)data[3] << 8U);
    value = (uint16_t)data[4] | ((uint16_t)data[5] << 8U);

    if (((operation == APP_ONEWIRE_OPERATION_REHANDSHAKE) &&
         ((address != 0U) || (value != 0U))) ||
        ((operation == APP_ONEWIRE_OPERATION_READ) && (value != 0U)))
    {
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_ONEWIRE);
        return;
    }

    submit_result = AppOneWire_Submit(operation, address, value);
    status = onewire_submit_status(submit_result);
    send_error(TYPE_CONTROL, status, OBJ_ONEWIRE);
}

static void write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
}

static void write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
    dst[2] = (uint8_t)(value >> 16U);
    dst[3] = (uint8_t)(value >> 24U);
}

static void write_i32_le(uint8_t *dst, int32_t value)
{
    write_u32_le(dst, (uint32_t)value);
}

static void process_damper_query(void)
{
    DamperSnapshot snapshot;
    uint8_t d[23];

    AppDamper_GetSnapshot(&snapshot);

    d[0] = STATUS_OK;
    d[1] = OBJ_DAMPER;
    d[2] = snapshot.damper_state;
    d[3] = snapshot.flags;
    write_i32_le(&d[4],  snapshot.current_steps);
    write_i32_le(&d[8],  snapshot.target_steps);
    write_u32_le(&d[12], snapshot.remaining_steps);
    write_u16_le(&d[16], snapshot.full_travel_steps);
    write_u16_le(&d[18], snapshot.configured_pps);
    d[20] = snapshot.last_command;
    d[21] = snapshot.last_result;
    d[22] = snapshot.fault_flags;

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

static int32_t damper_read_int32le(const uint8_t *data, uint8_t offset)
{
    uint32_t raw = (uint32_t)data[offset] |
                   ((uint32_t)data[offset + 1U] << 8U) |
                   ((uint32_t)data[offset + 2U] << 16U) |
                   ((uint32_t)data[offset + 3U] << 24U);
    return (int32_t)raw;
}

static uint8_t damper_status_to_w2(DamperStatus damper_status)
{
    switch (damper_status)
    {
    case DAMPER_STATUS_OK:            return STATUS_OK;
    case DAMPER_STATUS_BUSY:          return STATUS_BUSY;
    case DAMPER_STATUS_PARAM_RANGE:   return STATUS_PARAM_RANGE;
    case DAMPER_STATUS_NO_VALID_DATA: return STATUS_NO_VALID_DATA;
    case DAMPER_STATUS_READ_ONLY:     return STATUS_READ_ONLY;
    case DAMPER_STATUS_HW_ERROR:
    default:                          return STATUS_HW_ERROR;
    }
}

static void process_damper_control(const uint8_t *data, uint8_t len)
{
    uint8_t  sub_cmd;
    uint8_t  damper_status;
    int32_t  value;

    if (len < 2U)
    {
        send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
        return;
    }

    sub_cmd = data[1];

    switch (sub_cmd)
    {
    case DAMPER_A2_MOVE_ABSOLUTE:
        if (len != 6U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            return;
        }
        value = damper_read_int32le(data, 2U);
        damper_status = AppDamper_MoveAbsolute(value);
        break;

    case DAMPER_A2_MOVE_RELATIVE:
        if (len != 6U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            return;
        }
        value = damper_read_int32le(data, 2U);
        damper_status = AppDamper_MoveRelative(value);
        break;

    case DAMPER_A2_STOP:
        if (len != 2U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            return;
        }
        damper_status = AppDamper_Stop();
        break;

    case DAMPER_A2_RELEASE:
        if (len != 2U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            return;
        }
        damper_status = AppDamper_Release();
        break;

    case DAMPER_A2_SET_CURRENT_POSITION:
        if (len != 6U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            return;
        }
        value = damper_read_int32le(data, 2U);
        damper_status = AppDamper_SetCurrentPosition(value);
        break;

    default:
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_DAMPER);
        return;
    }

    send_error(TYPE_CONTROL, damper_status_to_w2(damper_status), OBJ_DAMPER);
}

/* PD15 drives an inverting NPN collector output:
   PD15 HIGH -> transistor ON  -> collector output LOW.
   PD15 LOW  -> transistor OFF -> collector output HIGH
                when external collector pull-up/load is present.
   Software returns the PD15 pin drive state (ODR), not collector level. */

static bool pd13_get(void)
{
    return ((GPIOD->ODR & GPIO_PIN_13) != 0U);
}

static bool pd13_set(bool high)
{
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13,
        high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return (((GPIOD->ODR & GPIO_PIN_13) != 0U) == high);
}

static void process_a1_query(const uint8_t *data, uint8_t len)
{
    if (len < 1U) { send_error(TYPE_QUERY, STATUS_LENGTH_ERROR, 0x00U); return; }
    uint8_t obj = data[0];

    switch (obj)
    {
    case OBJ_PWM_OUT:
    {
        uint8_t d[9];
        d[0] = STATUS_OK;
        d[1] = OBJ_PWM_OUT;
        d[2] = AppPwm_IsEnabled() ? 1U : 0U;
        uint32_t freq = AppPwm_GetActualFrequency();
        uint16_t duty = AppPwm_IsEnabled() ? AppPwm_GetActualDutyX100()
                                           : AppPwm_GetTargetDutyX100();
        d[3] = (uint8_t)freq;
        d[4] = (uint8_t)(freq >> 8);
        d[5] = (uint8_t)(freq >> 16);
        d[6] = (uint8_t)(freq >> 24);
        d[7] = (uint8_t)duty;
        d[8] = (uint8_t)(duty >> 8);
        SendW2Frame(TYPE_QUERY, d, 9U);
        break;
    }
    case OBJ_LED:
    {
        uint8_t d[4];
        d[0] = STATUS_OK;
        d[1] = OBJ_LED;
        d[2] = (uint8_t)AppLed_GetMode();
        d[3] = AppLed_IsOn() ? 1U : 0U;
        SendW2Frame(TYPE_QUERY, d, 4U);
        break;
    }
    case OBJ_PWM_IN:
    {
        AppPwmInputSnapshot snap;
        if (!AppPwmInput_GetSnapshot(&snap))
        {
            send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_PWM_IN);
            return;
        }
        uint8_t d[11];
        d[0] = STATUS_OK;
        d[1] = OBJ_PWM_IN;
        d[2] = (uint8_t)snap.status;
        uint32_t fm = (snap.status == APP_PWM_IN_OK) ? snap.freq_millihz : 0U;
        uint16_t dx = (snap.status == APP_PWM_IN_OK) ? snap.duty_x100 : 0U;
        d[3] = (uint8_t)fm;
        d[4] = (uint8_t)(fm >> 8);
        d[5] = (uint8_t)(fm >> 16);
        d[6] = (uint8_t)(fm >> 24);
        d[7] = (uint8_t)dx;
        d[8] = (uint8_t)(dx >> 8);
        uint16_t age = (snap.age_ms > 65535U) ? 65535U : (uint16_t)snap.age_ms;
        d[9]  = (uint8_t)age;
        d[10] = (uint8_t)(age >> 8);
        SendW2Frame(TYPE_QUERY, d, 11U);
        break;
    }
    case OBJ_PD13:
    {
        uint8_t d[3];
        d[0] = STATUS_OK;
        d[1] = OBJ_PD13;
        d[2] = pd13_get() ? 0x01U : 0x00U;
        SendW2Frame(TYPE_QUERY, d, 3U);
        break;
    }
    case OBJ_FAN:
    {
        AppFanSnapshot s;
        if (!AppFan_GetSnapshot(&s))
        {
            send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_FAN);
            return;
        }
        uint8_t d[18];
        d[0]  = STATUS_OK;
        d[1]  = OBJ_FAN;
        d[2]  = (uint8_t)s.state;
        d[3]  = s.enabled ? 1U : 0U;
        d[4]  = (uint8_t)s.target_duty_x100;
        d[5]  = (uint8_t)(s.target_duty_x100 >> 8);
        d[6]  = (uint8_t)s.applied_duty_x100;
        d[7]  = (uint8_t)(s.applied_duty_x100 >> 8);
        d[8]  = (uint8_t)s.pwm_frequency_hz;
        d[9]  = (uint8_t)(s.pwm_frequency_hz >> 8);
        d[10] = (uint8_t)s.fg_frequency_millihz;
        d[11] = (uint8_t)(s.fg_frequency_millihz >> 8);
        d[12] = (uint8_t)(s.fg_frequency_millihz >> 16);
        d[13] = (uint8_t)(s.fg_frequency_millihz >> 24);
        d[14] = (uint8_t)s.rpm;
        d[15] = (uint8_t)(s.rpm >> 8);
        d[16] = (uint8_t)s.tach_age_ms;
        d[17] = (uint8_t)(s.tach_age_ms >> 8);
        SendW2Frame(TYPE_QUERY, d, 18U);
        break;
    }
    case OBJ_NTC:
    {
        AppNtcSnapshot s;
        if (!AppNtc_GetSnapshot(&s))
        {
            send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_NTC);
            return;
        }
        uint8_t d[15];
        d[0] = STATUS_OK;
        d[1] = OBJ_NTC;
        d[2] = (uint8_t)s.state;
        d[3] = (uint8_t)s.adc_raw;
        d[4] = (uint8_t)(s.adc_raw >> 8);
        d[5] = (uint8_t)s.voltage_mv;
        d[6] = (uint8_t)(s.voltage_mv >> 8);
        d[7]  = (uint8_t)s.resistance_ohm;
        d[8]  = (uint8_t)(s.resistance_ohm >> 8);
        d[9]  = (uint8_t)(s.resistance_ohm >> 16);
        d[10] = (uint8_t)(s.resistance_ohm >> 24);
        d[11] = (uint8_t)s.temp_centi_c;
        d[12] = (uint8_t)(s.temp_centi_c >> 8);
        d[13] = (uint8_t)s.age_ms;
        d[14] = (uint8_t)(s.age_ms >> 8);
        SendW2Frame(TYPE_QUERY, d, 15U);
        break;
    }
    case OBJ_ONEWIRE:
        process_onewire_query();
        break;
    case OBJ_DAMPER:
        if (len != 1U)
        {
            send_error(TYPE_QUERY, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            break;
        }
        process_damper_query();
        break;
    default:
        send_error(TYPE_QUERY, STATUS_UNSUPPORTED_OBJECT, obj);
        break;
    }
}

static void process_a2_control(const uint8_t *data, uint8_t len)
{
    if (len < 1U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, 0x00U); return; }
    uint8_t obj = data[0];

    switch (obj)
    {
    case OBJ_PWM_OUT:
    {
        if (len < 8U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_PWM_OUT); return; }
        uint8_t en = data[1];
        if (en > 1U) { send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PWM_OUT); return; }

        if (en == 1U)
        {
            uint32_t freq = (uint32_t)data[2] |
                           ((uint32_t)data[3] << 8) |
                           ((uint32_t)data[4] << 16) |
                           ((uint32_t)data[5] << 24);
            uint16_t duty = (uint16_t)data[6] | ((uint16_t)data[7] << 8);

            if ((freq < 1U) || (freq > 100000U) || (duty > 10000U))
            {
                send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PWM_OUT);
                return;
            }
            if (!AppPwm_SetFrequency(freq) ||
                !AppPwm_SetDutyX100(duty))
            {
                send_error(TYPE_CONTROL, STATUS_APPLY_FAILED, OBJ_PWM_OUT);
                return;
            }
            if (!AppPwm_Enable(true))
            {
                send_error(TYPE_CONTROL, STATUS_APPLY_FAILED, OBJ_PWM_OUT);
                return;
            }
        }
        else
        {
            uint32_t fz = (uint32_t)data[2] |
                         ((uint32_t)data[3] << 8) |
                         ((uint32_t)data[4] << 16) |
                         ((uint32_t)data[5] << 24);
            uint16_t dz = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
            if ((fz != 0U) || (dz != 0U))
            {
                send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PWM_OUT);
                return;
            }
            if (!AppPwm_Enable(false))
            {
                send_error(TYPE_CONTROL, STATUS_APPLY_FAILED, OBJ_PWM_OUT);
                return;
            }
        }
        uint8_t r[2];
        r[0] = STATUS_OK;
        r[1] = OBJ_PWM_OUT;
        SendW2Frame(TYPE_CONTROL, r, 2U);
        break;
    }
    case OBJ_LED:
    {
        if (len < 2U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_LED); return; }
        uint8_t cmd = data[1];
        if (cmd > 2U) { send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_LED); return; }

        if (cmd == 2U)
        {
            AppLed_SetAutomatic();
        }
        else
        {
            AppLed_SetManual(cmd == 1U);
        }

        uint8_t r[2];
        r[0] = STATUS_OK;
        r[1] = OBJ_LED;
        SendW2Frame(TYPE_CONTROL, r, 2U);
        break;
    }
    case OBJ_PD13:
    {
        if (len < 2U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_PD13); return; }
        uint8_t lvl = data[1];
        if ((lvl != 0x00U) && (lvl != 0x01U))
        {
            send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PD13);
            return;
        }
        if (!pd13_set(lvl == 0x01U))
        {
            send_error(TYPE_CONTROL, STATUS_APPLY_FAILED, OBJ_PD13);
            return;
        }
        uint8_t r[2];
        r[0] = STATUS_OK;
        r[1] = OBJ_PD13;
        SendW2Frame(TYPE_CONTROL, r, 2U);
        break;
    }
    case OBJ_FAN:
    {
        if (len < 4U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_FAN); return; }
        uint8_t en = data[1];
        if (en > 1U) { send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_FAN); return; }
        uint16_t dty = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
        if (!AppFan_SetEnabled(en == 1U, dty))
        {
            send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_FAN);
            return;
        }
        uint8_t r[2];
        r[0] = STATUS_OK;
        r[1] = OBJ_FAN;
        SendW2Frame(TYPE_CONTROL, r, 2U);
        break;
    }
    case OBJ_NTC:
        send_error(TYPE_CONTROL, STATUS_READ_ONLY, OBJ_NTC);
        break;
    case OBJ_PWM_IN:
        send_error(TYPE_CONTROL, STATUS_READ_ONLY, OBJ_PWM_IN);
        break;
    case OBJ_ONEWIRE:
        process_onewire_control(data, len);
        break;
    case OBJ_DAMPER:
        process_damper_control(data, len);
        break;
    default:
        send_error(TYPE_CONTROL, STATUS_UNSUPPORTED_OBJECT, obj);
        break;
    }
}

static void dispatch_frame(void)
{
    if ((frame_len < 2U) || (frame_len > FRAME_MAX_LEN))
    {
        return;
    }

    uint8_t type = frame_buf[0];
    uint8_t len_field = frame_buf[1];
    uint8_t data_len = len_field - 2U;

    if (data_len > FRAME_MAX_DATA) { return; }

    uint8_t csum = type + len_field;
    for (uint8_t i = 0U; i < data_len; i++)
    {
        csum += frame_buf[2U + i];
    }
    if (csum != frame_buf[2U + data_len]) { return; }

    const uint8_t *d = &frame_buf[2U];

    if (type == TYPE_QUERY)        { process_a1_query(d, data_len); }
    else if (type == TYPE_CONTROL)  { process_a2_control(d, data_len); }
}

static void handle_byte(uint8_t byte)
{
    last_byte_tick = HAL_GetTick();

    switch (parser_state)
    {
    case PARSER_WAIT_HEADER:
        if (byte == FRAME_HEADER)
        {
            parser_state = PARSER_READ_TYPE;
            frame_idx    = 0U;
            frame_len    = 0U;
        }
        break;

    case PARSER_READ_TYPE:
        frame_buf[frame_idx++] = byte;
        parser_state = PARSER_READ_LENGTH;
        break;

    case PARSER_READ_LENGTH:
        frame_len = byte;
        if ((frame_len < 2U) || (frame_len > FRAME_MAX_LEN))
        {
            reset_parser();
            break;
        }
        frame_buf[frame_idx++] = byte;
        parser_state = PARSER_READ_DATA;
        break;

    case PARSER_READ_DATA:
    {
        uint8_t needed = frame_len - 2U;
        frame_buf[frame_idx++] = byte;
        if (frame_idx >= (2U + needed))
        {
            parser_state = PARSER_READ_CHECKSUM;
        }
        break;
    }

    case PARSER_READ_CHECKSUM:
        frame_buf[frame_idx++] = byte;
        dispatch_frame();
        reset_parser();
        break;
    }
}

void AppUart_Init(void)
{
    rx_head    = 0U;
    rx_tail    = 0U;
    rx_overflow = false;
    reset_parser();
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1U);
}

void AppUart_Process(void)
{
    if (parser_state != PARSER_WAIT_HEADER)
    {
        if ((HAL_GetTick() - last_byte_tick) > TIMEOUT_MS)
        {
            reset_parser();
        }
    }

    while (!ring_is_empty())
    {
        uint8_t byte = ring_get();
        handle_byte(byte);
    }

    if (rx_overflow)
    {
        rx_overflow = false;
        reset_parser();
    }
}

void AppUart_RxCpltCallback(void)
{
    if (ring_is_full())
    {
        rx_overflow = true;
    }
    else
    {
        ring_put(rx_byte);
    }
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1U);
}

void AppUart_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) { return; }
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1U);
}
