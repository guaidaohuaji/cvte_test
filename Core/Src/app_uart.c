/**
 * @file app_uart.c
 * @brief W2 上位机二进制协议入口（实现文件）。
 *
 * 模块职责：接收 USART1 字节流，解析 7E/A1/A2 帧，按对象号分发查询或控制命令，并把各业务模块的快照编码成 W2 应答。
 * 数据输入：USART1 RX 中断写入的环形缓冲；各业务模块公开的查询接口。
 * 数据输出：USART1 二进制应答；对 PWM、LED、风机、风门、AUTO 和单总线模块的控制调用。
 * 执行上下文：RX 中断只收一个字节并重新使能接收；完整解析、参数检查和应答均在主循环 AppUart_Process() 中完成。
 * 阅读重点：先看对象号和状态码宏，再看 dispatch_frame()，最后按对象进入 process_*_query/control()。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_uart.h"
#include "app_pwm.h"
#include "app_pwm_input.h"
#include "app_ntc.h"
#include "app_fan.h"
#include "app_onewire.h"
#include "app_onewire_config.h"
#include "app_led.h"
#include "app_damper.h"
#include "app_auto_control.h"
#include "app_manual_fan_control.h"
#include "app_fan_health.h"
#include "usart.h"
#include <stdbool.h>

#define RING_SIZE   128U
#define RING_MASK   (RING_SIZE - 1U)
#define FRAME_MAX_DATA  32U
#define FRAME_MAX_LEN   (FRAME_MAX_DATA + 2U)
#define FRAME_BUF_SIZE  36U
#define TIMEOUT_MS      50U

/* W2 帧：7E | TYPE | LENGTH | DATA | CHECKSUM。
 * LENGTH = DATA长度+2；CHECKSUM 为 TYPE+LENGTH+DATA 的低 8 位。 */
#define TYPE_QUERY   0xA1U
#define TYPE_CONTROL 0xA2U
#define FRAME_HEADER 0x7EU

/* 对象号是协议层与业务模块之间的稳定边界。新增对象时应同时更新
 * 查询、控制、协议文档、上位机解析和测试向量。 */
#define OBJ_PWM_OUT  0x01U
#define OBJ_LED      0x02U
#define OBJ_PWM_IN   0x03U
#define OBJ_PD13     0x04U
#define OBJ_NTC      0x05U
#define OBJ_FAN      0x06U
#define OBJ_ONEWIRE  0x08U
#define OBJ_DAMPER   0x07U
#define OBJ_AUTO_CONTROL 0x09U

#define DAMPER_A2_MOVE_ABSOLUTE        0x01U
#define DAMPER_A2_MOVE_RELATIVE        0x02U
#define DAMPER_A2_STOP                 0x03U
#define DAMPER_A2_RELEASE              0x04U
#define DAMPER_A2_SET_CURRENT_POSITION 0x05U

#define FAN_QUERY_EXTENDED_V1          0x01U
#define FAN_QUERY_HEALTH_V2            0x02U
#define FAN_EXTENDED_SCHEMA_VERSION    0x01U
#define FAN_HEALTH_SCHEMA_VERSION      0x02U
#define FAN_CONTROL_CLEAR_FAULT        0x03U

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
#define STATUS_MODE_LOCKED            0x0BU

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

/** @brief 判断 USART1 接收环形缓冲是否为空。 */
static bool ring_is_empty(void) { return rx_head == rx_tail; }
/** @brief 判断 USART1 接收环形缓冲是否已满；满时新字节会被标记为 overflow。 */
static bool ring_is_full(void)  { return ((rx_head + 1U) & RING_MASK) == rx_tail; }

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param byte 本次处理的接收字节。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void ring_put(uint8_t byte)
{
    uint16_t next = (rx_head + 1U) & RING_MASK;
    rx_ring[rx_head] = byte;
    rx_head = next;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint8_t ring_get(void)
{
    uint8_t byte = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1U) & RING_MASK;
    return byte;
}

/**
 * @brief 清除内部临时状态，使后续流程重新同步。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void reset_parser(void)
{
    parser_state = PARSER_WAIT_HEADER;
    frame_idx    = 0U;
    frame_len    = 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void send_binary(const uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 100U);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param type 见调用点；该参数只在本次调用期间有效。
 * @param data 输入数据缓冲区。
 * @param data_len 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param type 见调用点；该参数只在本次调用期间有效。
 * @param status 见调用点；该参数只在本次调用期间有效。
 * @param obj_id 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void send_error(uint8_t type, uint8_t status, uint8_t obj_id)
{
    uint8_t d[2];
    d[0] = status;
    d[1] = obj_id;
    SendW2Frame(type, d, 2U);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param result 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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
    case APP_ONEWIRE_SUBMIT_INVALID_SLAVE:
        return STATUS_PARAM_RANGE;
    case APP_ONEWIRE_SUBMIT_NOT_MASTER:
        return STATUS_READ_ONLY;
    case APP_ONEWIRE_SUBMIT_NO_CONTEXT:
        return STATUS_APPLY_FAILED;
    default:
        return STATUS_APPLY_FAILED;
    }
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param slave_address 目标从机地址。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool onewire_slave_address_is_valid(uint8_t slave_address)
{
    return (slave_address >= APP_ONEWIRE_SLAVE_ADDR_MIN) &&
           (slave_address <= APP_ONEWIRE_SLAVE_ADDR_MAX);
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_onewire_query(const uint8_t *data, uint8_t len)
{
    AppOneWireSnapshot snapshot;
    uint8_t slave_address;
    uint8_t d[16];

    if (len == 1U)
    {
        slave_address =
            (AppOneWire_GetRole() == APP_ONEWIRE_ROLE_VALUE_SLAVE)
            ? AppOneWire_GetLocalSlaveAddress()
            : APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS;
    }
    else if (len == 2U)
    {
        slave_address = data[1];
    }
    else
    {
        send_error(TYPE_QUERY, STATUS_LENGTH_ERROR, OBJ_ONEWIRE);
        return;
    }

    if (!onewire_slave_address_is_valid(slave_address))
    {
        send_error(TYPE_QUERY, STATUS_PARAM_RANGE, OBJ_ONEWIRE);
        return;
    }

    (void)AppOneWire_GetSnapshotForAddress(slave_address, &snapshot);

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
    d[12] = slave_address;
    d[13] = snapshot.context_valid ? 1U : 0U;
    d[14] = (uint8_t)snapshot.last_response_age_ms;
    d[15] = (uint8_t)(snapshot.last_response_age_ms >> 8U);

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_onewire_control(const uint8_t *data, uint8_t len)
{
    uint8_t slave_address;
    uint8_t operation;
    uint16_t address;
    uint16_t value;
    AppOneWireSubmitResult submit_result;
    uint8_t status;

    if (len == 6U)
    {
        slave_address = APP_ONEWIRE_DEFAULT_SLAVE_ADDRESS;
        operation = data[1];
        address = (uint16_t)data[2] | ((uint16_t)data[3] << 8U);
        value = (uint16_t)data[4] | ((uint16_t)data[5] << 8U);
    }
    else if (len == 7U)
    {
        slave_address = data[1];
        operation = data[2];
        address = (uint16_t)data[3] | ((uint16_t)data[4] << 8U);
        value = (uint16_t)data[5] | ((uint16_t)data[6] << 8U);
    }
    else
    {
        send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_ONEWIRE);
        return;
    }

    if (!onewire_slave_address_is_valid(slave_address) ||
        (((operation == APP_ONEWIRE_OPERATION_REHANDSHAKE) &&
          ((address != 0U) || (value != 0U))) ||
         ((operation == APP_ONEWIRE_OPERATION_READ) && (value != 0U))))
    {
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_ONEWIRE);
        return;
    }

    submit_result = AppOneWire_SubmitTo(
        slave_address, operation, address, value);
    status = onewire_submit_status(submit_result);
    send_error(TYPE_CONTROL, status, OBJ_ONEWIRE);
}

/**
 * @brief 按协议字节序写入整数。
 * @param dst 见调用点；该参数只在本次调用期间有效。
 * @param value 写入值，读取操作时通常为 0。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
}

/**
 * @brief 按协议字节序写入整数。
 * @param dst 见调用点；该参数只在本次调用期间有效。
 * @param value 写入值，读取操作时通常为 0。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8U);
    dst[2] = (uint8_t)(value >> 16U);
    dst[3] = (uint8_t)(value >> 24U);
}

/**
 * @brief 按协议字节序写入整数。
 * @param dst 见调用点；该参数只在本次调用期间有效。
 * @param value 写入值，读取操作时通常为 0。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void write_i32_le(uint8_t *dst, int32_t value)
{
    write_u32_le(dst, (uint32_t)value);
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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


/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t saturate_u32_to_u16(uint32_t value)
{
    return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

/**
 * @brief 按协议字节序写入整数。
 * @param dst 见调用点；该参数只在本次调用期间有效。
 * @param value 写入值，读取操作时通常为 0。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void write_i16_le(uint8_t *dst, int16_t value)
{
    write_u16_le(dst, (uint16_t)value);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param result 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint8_t manual_fan_result_to_w2(AppManualFanResult result)
{
    switch (result)
    {
    case APP_MANUAL_FAN_RESULT_OK:
        return STATUS_OK;
    case APP_MANUAL_FAN_RESULT_INVALID_PARAM:
        return STATUS_PARAM_RANGE;
    case APP_MANUAL_FAN_RESULT_MODE_LOCKED:
        return STATUS_MODE_LOCKED;
    case APP_MANUAL_FAN_RESULT_HW_ERROR:
    default:
        return STATUS_HW_ERROR;
    }
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_fan_legacy_query(void)
{
    AppFanSnapshot s;
    uint8_t d[18];

    if (!AppFan_GetSnapshot(&s))
    {
        send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_FAN);
        return;
    }

    d[0]  = STATUS_OK;
    d[1]  = OBJ_FAN;
    d[2]  = (uint8_t)s.state;
    d[3]  = s.enabled ? 1U : 0U;
    write_u16_le(&d[4], s.target_duty_x100);
    write_u16_le(&d[6], s.applied_duty_x100);
    write_u16_le(&d[8], s.pwm_frequency_hz);
    write_u32_le(&d[10], s.fg_frequency_millihz);
    write_u16_le(&d[14], s.rpm);
    write_u16_le(&d[16], s.tach_age_ms);

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_fan_extended_query(void)
{
    AppFanSnapshot fan;
    AppManualFanControlSnapshot manual;
    uint8_t d[32];
    uint8_t flags = 0U;
    uint8_t auto_mode;

    if (!AppFan_GetSnapshot(&fan) ||
        !AppManualFanControl_GetSnapshot(&manual))
    {
        send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_FAN);
        return;
    }

    auto_mode = AppAutoControl_GetMode();

    if (fan.enabled)
        flags |= 0x01U;
    if (fan.tach_valid)
        flags |= 0x02U;
    if (manual.tach_valid != 0U)
        flags |= 0x04U;
    if (manual.in_tolerance != 0U)
        flags |= 0x08U;
    if (auto_mode == APP_AUTO_MODE_AUTO)
        flags |= 0x10U;
    if (fan.state == APP_FAN_STATE_STARTUP_BOOST)
        flags |= 0x20U;
    if ((fan.state == APP_FAN_STATE_NO_TACH) ||
        (manual.control_state == APP_MANUAL_FAN_CTRL_TACH_FAULT))
    {
        flags |= 0x40U;
    }
    if ((fan.state == APP_FAN_STATE_PWM_ERROR) ||
        (fan.state == APP_FAN_STATE_CONFIG_ERROR) ||
        (fan.state == APP_FAN_STATE_SAFETY_LOCKED) ||
        (manual.control_state == APP_MANUAL_FAN_CTRL_HW_ERROR) ||
        (manual.control_state == APP_MANUAL_FAN_CTRL_SAFETY_LOCKED))
    {
        flags |= 0x80U;
    }

    d[0] = STATUS_OK;
    d[1] = OBJ_FAN;
    d[2] = FAN_EXTENDED_SCHEMA_VERSION;
    d[3] = manual.mode;
    d[4] = manual.control_state;
    d[5] = flags;
    write_u16_le(&d[6], manual.target_rpm);
    write_i16_le(&d[8], manual.rpm_error);
    write_u16_le(&d[10], manual.feedforward_duty_x100);
    write_u16_le(&d[12], fan.target_duty_x100);
    write_u16_le(&d[14], fan.applied_duty_x100);
    write_u16_le(&d[16], fan.pwm_frequency_hz);
    write_u32_le(&d[18], fan.fg_frequency_millihz);
    write_u16_le(&d[22], fan.rpm);
    write_u16_le(&d[24], fan.tach_age_ms);
    write_u16_le(&d[26], saturate_u32_to_u16(manual.adjust_count));
    write_u16_le(&d[28], saturate_u32_to_u16(manual.fault_count));
    d[30] = auto_mode;
    d[31] = 0U;

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_fan_health_query(void)
{
    AppFanSnapshot fan;
    AppFanHealthSnapshot health;
    uint8_t d[32];
    uint8_t flags = 0U;

    if (!AppFan_GetSnapshot(&fan) ||
        !AppFanHealth_GetSnapshot(&health))
    {
        send_error(TYPE_QUERY, STATUS_HW_ERROR, OBJ_FAN);
        return;
    }

    if (health.fault_latched != 0U)
        flags |= 0x01U;
    if (health.monitoring_active != 0U)
        flags |= 0x02U;
    if (health.tach_valid != 0U)
        flags |= 0x04U;
    if (health.shutdown_succeeded != 0U)
        flags |= 0x08U;
    if (health.restart_inhibited != 0U)
        flags |= 0x10U;
    if (fan.enabled)
        flags |= 0x20U;
    if (fan.state == APP_FAN_STATE_SAFETY_LOCKED)
        flags |= 0x40U;

    d[0] = STATUS_OK;
    d[1] = OBJ_FAN;
    d[2] = FAN_HEALTH_SCHEMA_VERSION;
    d[3] = health.state;
    d[4] = health.fault_type;
    d[5] = flags;
    write_u16_le(&d[6], health.applied_duty_x100);
    write_u16_le(&d[8], health.reference_duty_x100);
    write_u16_le(&d[10], health.expected_rpm);
    write_u16_le(&d[12], health.actual_rpm);
    write_i16_le(&d[14], health.deviation_rpm);
    write_u16_le(&d[16], health.absolute_deviation_rpm);
    write_u16_le(&d[18], health.maximum_absolute_deviation_rpm);
    write_u16_le(&d[20], health.fault_applied_duty_x100);
    write_u16_le(&d[22], health.fault_expected_rpm);
    write_u16_le(&d[24], health.fault_actual_rpm);
    write_i16_le(&d[26], health.fault_deviation_rpm);
    write_u16_le(&d[28], saturate_u32_to_u16(health.abnormal_elapsed_ms));
    write_u16_le(&d[30], saturate_u32_to_u16(health.settling_remaining_ms));

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_fan_query(const uint8_t *data, uint8_t len)
{
    /* Historical firmware ignored trailing query bytes for object 0x06.
       Preserve that behavior.  Only the exact two-byte selector below asks
       for the versioned extended response. */
    if (len == 2U)
    {
        if (data[1] == FAN_QUERY_EXTENDED_V1)
        {
            process_fan_extended_query();
            return;
        }
        if (data[1] == FAN_QUERY_HEALTH_V2)
        {
            process_fan_health_query();
            return;
        }
    }

    process_fan_legacy_query();
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_fan_control(const uint8_t *data, uint8_t len)
{
    uint8_t mode;
    uint16_t value;
    AppManualFanResult result;

    /* The clear-fault operation is deliberately available in both MANUAL
       and AUTO. It only clears the latch; restart remains inhibited until a
       subsequent explicit manual command or AUTO-mode command authorizes it. */
    if (len >= 2U && data[1] == FAN_CONTROL_CLEAR_FAULT)
    {
        if (len != 4U)
        {
            send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_FAN);
            return;
        }

        value = (uint16_t)data[2] | ((uint16_t)data[3] << 8U);
        if (value != 0U)
        {
            send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_FAN);
            return;
        }

        send_error(TYPE_CONTROL,
                   AppFanHealth_ClearFault() ? STATUS_OK : STATUS_HW_ERROR,
                   OBJ_FAN);
        return;
    }

    /* Preserve the historical priority for ordinary manual commands: AUTO
       ownership is reported before payload validation. */
    if (AppAutoControl_GetMode() != APP_AUTO_MODE_MANUAL)
    {
        send_error(TYPE_CONTROL, STATUS_MODE_LOCKED, OBJ_FAN);
        return;
    }

    /* Keep the legacy minimum-length rule so old senders with harmless
       trailing bytes remain compatible. */
    if (len < 4U)
    {
        send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_FAN);
        return;
    }

    mode = data[1];
    value = (uint16_t)data[2] | ((uint16_t)data[3] << 8U);

    switch (mode)
    {
    case APP_MANUAL_FAN_MODE_OFF:
        if (value != 0U)
        {
            send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_FAN);
            return;
        }
        result = AppManualFanControl_SetOff();
        break;

    case APP_MANUAL_FAN_MODE_DUTY:
        result = AppManualFanControl_SetDuty(value);
        break;

    case APP_MANUAL_FAN_MODE_SPEED:
        result = AppManualFanControl_SetTargetRpm(value);
        break;

    default:
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_FAN);
        return;
    }

    send_error(TYPE_CONTROL, manual_fan_result_to_w2(result), OBJ_FAN);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param data 输入数据缓冲区。
 * @param offset 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static int32_t damper_read_int32le(const uint8_t *data, uint8_t offset)
{
    uint32_t raw = (uint32_t)data[offset] |
                   ((uint32_t)data[offset + 1U] << 8U) |
                   ((uint32_t)data[offset + 2U] << 16U) |
                   ((uint32_t)data[offset + 3U] << 24U);
    return (int32_t)raw;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param damper_status 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool manual_control_is_allowed(void)
{
    return (AppAutoControl_GetMode() == APP_AUTO_MODE_MANUAL);
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_auto_control_query(void)
{
    AppAutoControlSnapshot snap;
    uint8_t d[28];

    AppAutoControl_GetSnapshot(&snap);

    d[0] = STATUS_OK;
    d[1] = OBJ_AUTO_CONTROL;
    d[2] = snap.mode;
    d[3] = snap.state;
    {
        uint8_t f = 0x00U;
        if (snap.flags & 0x01U) f |= 0x01U;
        if (snap.flags & 0x08U) f |= 0x02U;
        if (snap.flags & 0x10U) f |= 0x04U;
        if (snap.fan_tach_valid) f |= 0x08U;
        if (snap.flags & 0x80U) f |= 0x10U;
        if (snap.damper_position_valid) f |= 0x20U;
        if (snap.damper_control_state == APP_AUTO_DAMPER_CTRL_FAULT) f |= 0x40U;
        if (snap.damper_auto_owned) f |= 0x80U;
        d[4] = f;
    }
    d[5] = snap.fan_control_state;
    d[6] = snap.damper_control_state;
    d[7] = snap.ntc_range_status;
    {
        uint16_t u = (uint16_t)snap.control_temp_centi_c;
        d[8]  = (uint8_t)(u & 0xFFU);
        d[9]  = (uint8_t)(u >> 8U);
    }
    d[10] = (uint8_t)snap.target_fan_rpm;
    d[11] = (uint8_t)(snap.target_fan_rpm >> 8U);
    d[12] = (uint8_t)snap.actual_fan_rpm;
    d[13] = (uint8_t)(snap.actual_fan_rpm >> 8U);
    d[14] = (uint8_t)snap.applied_fan_duty_x100;
    d[15] = (uint8_t)(snap.applied_fan_duty_x100 >> 8U);
    {
        uint16_t u = (uint16_t)snap.target_damper_steps;
        d[16] = (uint8_t)(u & 0xFFU);
        d[17] = (uint8_t)(u >> 8U);
    }
    {
        uint16_t u = (uint16_t)snap.actual_damper_steps;
        d[18] = (uint8_t)(u & 0xFFU);
        d[19] = (uint8_t)(u >> 8U);
    }
    {
        uint16_t u = (uint16_t)snap.fan_error_rpm;
        d[20] = (uint8_t)(u & 0xFFU);
        d[21] = (uint8_t)(u >> 8U);
    }
    {
        uint16_t u = (uint16_t)snap.damper_error_steps;
        d[22] = (uint8_t)(u & 0xFFU);
        d[23] = (uint8_t)(u >> 8U);
    }
    d[24] = (uint8_t)snap.update_seq;
    d[25] = (uint8_t)(snap.update_seq >> 8U);
    d[26] = (uint8_t)(snap.update_seq >> 16U);
    d[27] = (uint8_t)(snap.update_seq >> 24U);

    (void)SendW2Frame(TYPE_QUERY, d, (uint8_t)sizeof(d));
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_auto_control_control(const uint8_t *data, uint8_t len)
{
    uint8_t new_mode;
    uint8_t result;

    if (len != 2U)
    {
        send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_AUTO_CONTROL);
        return;
    }

    new_mode = data[1];

    if (new_mode != APP_AUTO_MODE_MANUAL && new_mode != APP_AUTO_MODE_AUTO)
    {
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_AUTO_CONTROL);
        return;
    }

    result = AppAutoControl_SetMode(new_mode);

    if (result == APP_AUTO_SET_MODE_OK)
        send_error(TYPE_CONTROL, STATUS_OK, OBJ_AUTO_CONTROL);
    else if (result == APP_AUTO_SET_MODE_INVALID_PARAM)
        send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_AUTO_CONTROL);
    else if (result == APP_AUTO_SET_MODE_UNAVAILABLE)
        send_error(TYPE_CONTROL, STATUS_READ_ONLY, OBJ_AUTO_CONTROL);
    else
        send_error(TYPE_CONTROL, STATUS_HW_ERROR, OBJ_AUTO_CONTROL);
}

/* PD15 drives an inverting NPN collector output:
   PD15 HIGH -> transistor ON  -> collector output LOW.
   PD15 LOW  -> transistor OFF -> collector output HIGH
                when external collector pull-up/load is present.
   Software returns the PD15 pin drive state (ODR), not collector level. */

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool pd13_get(void)
{
    return ((GPIOD->ODR & GPIO_PIN_13) != 0U);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param high 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool pd13_set(bool high)
{
    HAL_GPIO_WritePin(GPIOD, GPIO_PIN_13,
        high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return (((GPIOD->ODR & GPIO_PIN_13) != 0U) == high);
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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
        process_fan_query(data, len);
        break;
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
        d[12] = (uint8_t)((uint16_t)s.temp_centi_c >> 8U);
        d[13] = (uint8_t)s.age_ms;
        d[14] = (uint8_t)(s.age_ms >> 8);
        SendW2Frame(TYPE_QUERY, d, 15U);
        break;
    }
    case OBJ_ONEWIRE:
        process_onewire_query(data, len);
        break;
    case OBJ_DAMPER:
        if (len != 1U)
        {
            send_error(TYPE_QUERY, STATUS_LENGTH_ERROR, OBJ_DAMPER);
            break;
        }
        process_damper_query();
        break;
    case OBJ_AUTO_CONTROL:
        if (len != 1U)
        {
            send_error(TYPE_QUERY, STATUS_LENGTH_ERROR, OBJ_AUTO_CONTROL);
            break;
        }
        process_auto_control_query();
        break;
    default:
        send_error(TYPE_QUERY, STATUS_UNSUPPORTED_OBJECT, obj);
        break;
    }
}

/**
 * @brief 处理当前状态下的一次事件或数据。
 * @param data 输入数据缓冲区。
 * @param len 数据长度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void process_a2_control(const uint8_t *data, uint8_t len)
{
    if (len < 1U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, 0x00U); return; }
    uint8_t obj = data[0];

    switch (obj)
    {
    case OBJ_PWM_OUT:
    {
        if (len != 8U) { send_error(TYPE_CONTROL, STATUS_LENGTH_ERROR, OBJ_PWM_OUT); return; }
        uint8_t en = data[1];
        if (en > 1U) { send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PWM_OUT); return; }

        if (en == 1U)
        {
            uint32_t freq = (uint32_t)data[2] |
                           ((uint32_t)data[3] << 8) |
                           ((uint32_t)data[4] << 16) |
                           ((uint32_t)data[5] << 24);
            uint16_t duty = (uint16_t)data[6] | ((uint16_t)data[7] << 8);

            if ((freq < APP_PWM_MIN_FREQUENCY_HZ) ||
                (freq > APP_PWM_MAX_FREQUENCY_HZ) || (duty > 10000U))
            {
                send_error(TYPE_CONTROL, STATUS_PARAM_RANGE, OBJ_PWM_OUT);
                return;
            }
            if (!AppPwm_Configure(true, freq, duty))
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
            if (!AppPwm_Configure(false, 0U, 0U))
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
        process_fan_control(data, len);
        break;
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
        if (!manual_control_is_allowed())
        {
            send_error(TYPE_CONTROL, STATUS_MODE_LOCKED, OBJ_DAMPER);
            break;
        }
        process_damper_control(data, len);
        break;
    case OBJ_AUTO_CONTROL:
        process_auto_control_control(data, len);
        break;
    default:
        send_error(TYPE_CONTROL, STATUS_UNSUPPORTED_OBJECT, obj);
        break;
    }
}

/**
 * @brief 验证并分发一帧已解析 W2 请求：A1 进入查询，A2 进入控制，其他类型返回统一错误。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief W2 字节级解析状态机，处理帧头、TYPE、LENGTH、DATA 和加和校验。
 * @param byte 本次处理的接收字节。
 * @note 文件内部辅助函数，不属于对外 API。
 */
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

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppUart_Init(void)
{
    rx_head    = 0U;
    rx_tail    = 0U;
    rx_overflow = false;
    reset_parser();
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1U);
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
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

/**
 * @brief 处理 HAL 单字节接收完成回调。
 */
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

/**
 * @brief 处理 HAL 错误回调并恢复底层接收状态。
 * @param huart 触发回调的 HAL UART 句柄。
 */
void AppUart_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART1) { return; }
    HAL_UART_Receive_IT(&huart1, (uint8_t *)&rx_byte, 1U);
}
