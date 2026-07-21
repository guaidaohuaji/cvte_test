#include "app_uart.h"
#include "app_pwm.h"
#include "app_pwm_input.h"
#include "app_ntc.h"
#include "app_fan.h"
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
    LED_MODE_AUTO_BLINK = 0,
    LED_MODE_MANUAL     = 1
} LedMode;

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

LedMode led_mode = LED_MODE_AUTO_BLINK;
extern uint32_t led_last_toggle_tick;
static bool    led_manual_on = false;

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
        d[2] = (uint8_t)led_mode;
        if (led_mode == LED_MODE_AUTO_BLINK)
        {
            uint8_t pin = (GPIOE->ODR & GPIO_PIN_8) ? 1U : 0U;
            d[3] = pin ? 0U : 1U;
        }
        else { d[3] = led_manual_on ? 1U : 0U; }
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
            led_mode = LED_MODE_AUTO_BLINK;
            led_last_toggle_tick = HAL_GetTick();
        }
        else
        {
            led_mode       = LED_MODE_MANUAL;
            led_manual_on  = (cmd == 1U);
            HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin,
                led_manual_on ? GPIO_PIN_RESET : GPIO_PIN_SET);
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
