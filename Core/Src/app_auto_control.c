/**
 * @file app_auto_control.c
 * @brief 温度映射的风机与风门自动控制（实现文件）。
 *
 * 模块职责：把有效温度映射为目标风机转速和风门位置，并分别驱动风机闭环和风门位置状态机。
 * 数据输入：NTC 快照、风机快照、风门快照、AUTO/MANUAL 模式命令。
 * 数据输出：目标 RPM、目标风门步数、执行命令和完整控制诊断快照。
 * 执行上下文：每秒评估目标；风机与风门控制各有独立节拍和状态，均为非阻塞。
 * 阅读重点：先看 evaluate_targets() 的线性映射，再分别看 fan_control_process() 与 damper_control_process()，最后看模式切换。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_auto_control.h"
#include "app_auto_fan_profile.h"
#include "app_ntc.h"
#include "app_fan.h"
#include "app_fan_health.h"
#include "app_damper.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

#define TEMP_SPAN_CENTI_C \
    (APP_AUTO_TEMP_MAX_CENTI_C - APP_AUTO_TEMP_MIN_CENTI_C)

#define RPM_SPAN \
    (APP_AUTO_FAN_MAX_RPM - APP_AUTO_FAN_MIN_RPM)

#define DAMPER_SPAN \
    (APP_AUTO_DAMPER_COLD_STEPS - APP_AUTO_DAMPER_HOT_STEPS)

/* Snapshot 同时承担运行状态和 W2 诊断数据源。写入集中在本文件，
 * 对外只复制，避免协议层直接操作控制内部变量。 */
static AppAutoControlSnapshot snap;
static AppAutoMode mode;
static AppAutoState state;
static AppAutoFanCtrlState fan_ctrl_state;
static AppAutoDamperCtrlState damper_ctrl_state;

static uint32_t last_process_tick;
static uint32_t auto_start_tick;
static uint32_t update_seq;

static uint32_t last_fan_adj_tick;
static uint32_t fan_adj_count;
static uint32_t fan_fault_count;

static uint16_t auto_fan_duty;
static uint16_t fan_feedforward_target_rpm;
static int8_t   fan_error_confirm_direction;
static uint8_t  fan_error_confirm_count;
static bool     fan_feedforward_pending;

static uint32_t last_damper_act_tick;
static uint32_t damper_cmd_count;
static uint32_t damper_busy_count;
static uint32_t damper_fault_count;
static bool auto_damper_owned;
static bool failsafe_stop_issued;
static bool damper_pos_invalid;

static bool init_ok;

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
__attribute__((unused))
static bool config_check(void)
{
    if (APP_AUTO_TEMP_MAX_CENTI_C <= APP_AUTO_TEMP_MIN_CENTI_C)
        return false;
    if (APP_AUTO_FAN_MAX_RPM < APP_AUTO_FAN_MIN_RPM)
        return false;
    if (APP_AUTO_DAMPER_COLD_STEPS < 0 ||
        APP_AUTO_DAMPER_COLD_STEPS > 1700)
        return false;
    if (APP_AUTO_DAMPER_HOT_STEPS < 0 ||
        APP_AUTO_DAMPER_HOT_STEPS > 1700)
        return false;
    if (APP_AUTO_DAMPER_COLD_STEPS <= APP_AUTO_DAMPER_HOT_STEPS)
        return false;
    if (APP_AUTO_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_TEMP_STARTUP_GRACE_MS == 0U)
        return false;
    if (APP_AUTO_FAN_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_FAN_TOLERANCE_ENTER_RPM == 0U ||
        APP_AUTO_FAN_TOLERANCE_EXIT_RPM < APP_AUTO_FAN_TOLERANCE_ENTER_RPM)
        return false;
    if (APP_AUTO_FAN_FEEDFORWARD_DELTA_RPM == 0U ||
        APP_AUTO_FAN_ERROR_CONFIRM_CYCLES == 0U)
        return false;
    if (APP_AUTO_FAN_FINE_ERROR_MAX_RPM == 0U ||
        APP_AUTO_FAN_MEDIUM_ERROR_MAX_RPM < APP_AUTO_FAN_FINE_ERROR_MAX_RPM)
        return false;
    if (APP_AUTO_FAN_FINE_STEP_X100 == 0U ||
        APP_AUTO_FAN_MEDIUM_STEP_X100 < APP_AUTO_FAN_FINE_STEP_X100 ||
        APP_AUTO_FAN_LARGE_STEP_X100 < APP_AUTO_FAN_MEDIUM_STEP_X100)
        return false;
    if (APP_AUTO_FAN_DUTY_QUANTUM_X100 == 0U)
        return false;
    if (APP_AUTO_FAN_NORMAL_MIN_DUTY_X100 < APP_FAN_MIN_DUTY_X100 ||
        APP_AUTO_FAN_NORMAL_MAX_DUTY_X100 > APP_FAN_MAX_DUTY_X100 ||
        APP_AUTO_FAN_NORMAL_MIN_DUTY_X100 >= APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        return false;
    if (APP_AUTO_FAN_FAILSAFE_DUTY_X100 < APP_FAN_MIN_DUTY_X100 ||
        APP_AUTO_FAN_FAILSAFE_DUTY_X100 > APP_FAN_MAX_DUTY_X100 ||
        APP_AUTO_FAN_FAILSAFE_DUTY_X100 < APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        return false;
    if (AppAutoFan_EstimateDutyX100(APP_AUTO_FAN_MIN_RPM) !=
        APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        return false;
    if (AppAutoFan_EstimateDutyX100(APP_AUTO_FAN_MAX_RPM) !=
        APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        return false;
    if (APP_AUTO_DAMPER_CONTROL_PERIOD_MS == 0U)
        return false;
    if (APP_AUTO_DAMPER_DEADBAND_STEPS == 0U ||
        APP_AUTO_DAMPER_DEADBAND_STEPS > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return false;
    if (APP_AUTO_DAMPER_FAILSAFE_STEPS < 0 ||
        (uint32_t)APP_AUTO_DAMPER_FAILSAFE_STEPS > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        return false;
    return true;
}

/**
 * @brief 把控制温度在配置范围内线性映射为目标风机 RPM 和风门步数；低温对应关闭，高温对应打开。
 * @param control_temp 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void evaluate_targets(int16_t control_temp)
{
    int16_t clamped = control_temp;
    uint32_t rpm, damper;

    if (clamped < APP_AUTO_TEMP_MIN_CENTI_C)
        clamped = APP_AUTO_TEMP_MIN_CENTI_C;
    if (clamped > APP_AUTO_TEMP_MAX_CENTI_C)
        clamped = APP_AUTO_TEMP_MAX_CENTI_C;

    {
        int32_t temp_delta = (int32_t)clamped - APP_AUTO_TEMP_MIN_CENTI_C;

        int64_t rpm_num = (int64_t)temp_delta * (int64_t)RPM_SPAN
                        + (int64_t)(TEMP_SPAN_CENTI_C / 2);
        rpm = APP_AUTO_FAN_MIN_RPM
            + (uint32_t)(rpm_num / (int64_t)TEMP_SPAN_CENTI_C);

        int64_t damper_num = (int64_t)temp_delta * (int64_t)DAMPER_SPAN
                           + (int64_t)(TEMP_SPAN_CENTI_C / 2);
        damper = (uint32_t)APP_AUTO_DAMPER_COLD_STEPS
               - (uint32_t)(damper_num / (int64_t)TEMP_SPAN_CENTI_C);
    }

    if (rpm < APP_AUTO_FAN_MIN_RPM) rpm = APP_AUTO_FAN_MIN_RPM;
    if (rpm > APP_AUTO_FAN_MAX_RPM) rpm = APP_AUTO_FAN_MAX_RPM;
    if (damper > (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        damper = (uint32_t)APP_DAMPER_FULL_TRAVEL_STEPS;

    snap.target_fan_rpm     = (uint16_t)rpm;
    snap.target_damper_steps = (int32_t)damper;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
__attribute__((unused))
static void do_evaluation(void)
{
    AppNtcSnapshot ntc;
    bool temp_valid;

    if (!AppNtc_GetSnapshot(&ntc))
    {
        snap.flags &= ~0x01U;
        return;
    }

    snap.ntc_state        = (uint8_t)ntc.state;
    snap.ntc_range_status = (uint8_t)ntc.range_status;
    snap.measured_temp_centi_c  = ntc.temp_centi_c;
    snap.control_temp_centi_c   = ntc.control_temp_centi_c;

    temp_valid = ntc.sensor_measurement_valid;

    {
        uint8_t f = snap.flags & 0xC0U;

        if (temp_valid) f |= 0x01U;
        if (ntc.range_status == APP_NTC_RANGE_CLAMPED_LOW)  f |= 0x02U;
        if (ntc.range_status == APP_NTC_RANGE_CLAMPED_HIGH) f |= 0x04U;

        snap.flags = f;
    }

    if (!temp_valid)
    {
        uint32_t now = HAL_GetTick();

        if ((uint32_t)(now - auto_start_tick)
            < APP_AUTO_TEMP_STARTUP_GRACE_MS)
        {
            state = APP_AUTO_STATE_WAIT_TEMP;
            snap.flags &= ~0x08U;
        }
        else
        {
            state = APP_AUTO_STATE_TEMP_FAULT;
            snap.target_fan_rpm      = APP_AUTO_FAN_MAX_RPM;
            snap.target_damper_steps  = APP_AUTO_DAMPER_HOT_STEPS;
            snap.flags |= 0x18U;
        }
    }
    else
    {
        state = APP_AUTO_STATE_TARGET_READY;
        snap.flags |= 0x08U;
        snap.flags &= ~0x10U;

        evaluate_targets(ntc.control_temp_centi_c);
    }

    update_seq++;
    snap.update_seq      = update_seq;
    snap.last_update_tick = HAL_GetTick();
    snap.state            = (uint8_t)state;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
__attribute__((unused))
static bool targets_are_valid(void)
{
    return (snap.flags & 0x08U) != 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param new_state 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fan_set_ctrl_state(AppAutoFanCtrlState new_state)
{
    fan_ctrl_state = new_state;

    if (new_state == APP_AUTO_FAN_CTRL_IN_TOLERANCE)
        snap.flags |= 0x40U;
    else
        snap.flags &= ~0x40U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fan_reset_error_confirmation(void)
{
    fan_error_confirm_direction = 0;
    fan_error_confirm_count = 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param error 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t fan_abs_error_rpm(int32_t error)
{
    uint32_t magnitude = (error < 0) ? (uint32_t)(-error) : (uint32_t)error;
    return (magnitude > 65535U) ? 65535U : (uint16_t)magnitude;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param new_duty 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool fan_set_duty(uint16_t new_duty)
{
    if (new_duty < APP_FAN_MIN_DUTY_X100)
        new_duty = APP_FAN_MIN_DUTY_X100;
    if (new_duty > APP_FAN_MAX_DUTY_X100)
        new_duty = APP_FAN_MAX_DUTY_X100;

    if (new_duty == auto_fan_duty)
        return true;

    if (AppFan_SetEnabled(true, new_duty))
    {
        auto_fan_duty = new_duty;
        snap.fan_control_result = 0U;
        return true;
    }

    fan_set_ctrl_state(APP_AUTO_FAN_CTRL_HW_ERROR);
    snap.fan_control_result = 1U;
    snap.flags |= 0x80U;
    return false;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param new_duty 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool fan_set_normal_duty(uint16_t new_duty)
{
    if (new_duty < APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        new_duty = APP_AUTO_FAN_NORMAL_MIN_DUTY_X100;
    if (new_duty > APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        new_duty = APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;
    return fan_set_duty(new_duty);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param target_rpm 目标转速，单位 RPM。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool fan_feedforward_is_needed(uint16_t target_rpm)
{
    uint16_t delta;

    if (fan_feedforward_pending || fan_feedforward_target_rpm == 0U)
        return true;

    delta = (target_rpm >= fan_feedforward_target_rpm)
          ? (uint16_t)(target_rpm - fan_feedforward_target_rpm)
          : (uint16_t)(fan_feedforward_target_rpm - target_rpm);

    return delta >= APP_AUTO_FAN_FEEDFORWARD_DELTA_RPM;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param target_rpm 目标转速，单位 RPM。
 * @param now 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool fan_apply_feedforward(uint16_t target_rpm, uint32_t now)
{
    uint16_t estimated_duty = AppAutoFan_EstimateDutyX100(target_rpm);

    if (!fan_set_normal_duty(estimated_duty))
        return false;

    fan_feedforward_target_rpm = target_rpm;
    fan_feedforward_pending = false;
    fan_reset_error_confirmation();
    last_fan_adj_tick = now;

    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param direction 见调用点；该参数只在本次调用期间有效。
 * @param absolute_error_rpm 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool fan_error_direction_is_confirmed(int8_t direction,
                                             uint16_t absolute_error_rpm)
{
    if (absolute_error_rpm > APP_AUTO_FAN_MEDIUM_ERROR_MAX_RPM)
    {
        fan_reset_error_confirmation();
        return true;
    }

    if (fan_error_confirm_direction != direction)
    {
        fan_error_confirm_direction = direction;
        fan_error_confirm_count = 1U;
        return APP_AUTO_FAN_ERROR_CONFIRM_CYCLES <= 1U;
    }

    if (fan_error_confirm_count < UINT8_MAX)
        fan_error_confirm_count++;

    if (fan_error_confirm_count >= APP_AUTO_FAN_ERROR_CONFIRM_CYCLES)
    {
        fan_reset_error_confirmation();
        return true;
    }

    return false;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void fan_publish_control_diag(void)
{
    snap.fan_adjust_count = fan_adj_count;
    snap.fan_fault_count = fan_fault_count;
    snap.last_fan_control_tick = last_fan_adj_tick;
    snap.fan_control_state = (uint8_t)fan_ctrl_state;
}

/**
 * @brief AUTO 风机子状态机：前馈启动、等待有效测速、误差方向确认、1/2/3%调节、容差迟滞和故障处理。
 * @note 文件内部辅助函数，不属于对外 API。
 */
__attribute__((unused))
static void fan_control_process(void)
{
    AppFanSnapshot fan_snap;
    uint16_t current_duty;
    uint16_t commanded_duty;
    uint16_t absolute_error;
    uint16_t trim_step;
    uint16_t new_duty;
    bool fan_enabled;
    bool tach_ok;
    bool was_in_tolerance;
    int8_t direction;
    int32_t error;
    uint32_t now;

    if (!AppFan_GetSnapshot(&fan_snap))
        return;

    now = HAL_GetTick();
    fan_enabled = fan_snap.enabled;
    current_duty = fan_snap.applied_duty_x100;
    commanded_duty = fan_snap.target_duty_x100;
    tach_ok = (fan_snap.tach_valid != 0U);

    snap.fan_state = (uint8_t)fan_snap.state;
    snap.fan_tach_valid = tach_ok ? 1U : 0U;
    snap.actual_fan_rpm = fan_snap.rpm;
    snap.applied_fan_duty_x100 = current_duty;

    auto_fan_duty = fan_enabled ? commanded_duty : 0U;

    if (AppFanHealth_IsFaultLatched() ||
        AppFanHealth_RestartIsInhibited())
    {
        auto_fan_duty = 0U;
        fan_feedforward_pending = true;
        fan_reset_error_confirmation();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_SAFETY_LOCKED);
        goto done;
    }

    if (!fan_enabled)
    {
        fan_feedforward_pending = true;
        if (fan_apply_feedforward(snap.target_fan_rpm, now))
            fan_set_ctrl_state(APP_AUTO_FAN_CTRL_STARTING);
        goto done;
    }

    if (fan_snap.state == APP_FAN_STATE_STARTUP_BOOST)
    {
        /* Keep the physical output at 100%, but update the post-boost target. */
        if (fan_feedforward_is_needed(snap.target_fan_rpm))
            (void)fan_apply_feedforward(snap.target_fan_rpm, now);

        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_STARTING);
        last_fan_adj_tick = now;
        goto done;
    }

    if (current_duty == 0U)
    {
        fan_feedforward_pending = true;
        fan_reset_error_confirmation();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
        goto done;
    }

    if (fan_snap.state == APP_FAN_STATE_NO_TACH)
    {
        if (commanded_duty != APP_AUTO_FAN_FAILSAFE_DUTY_X100)
        {
            if (fan_set_duty(APP_AUTO_FAN_FAILSAFE_DUTY_X100) &&
                fan_fault_count != UINT32_MAX)
            {
                fan_fault_count++;
            }
        }
        fan_reset_error_confirmation();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_TACH_FAULT);
        last_fan_adj_tick = now;
        goto done;
    }

    if (fan_ctrl_state == APP_AUTO_FAN_CTRL_TACH_FAULT)
    {
        if (!tach_ok)
        {
            last_fan_adj_tick = now;
            goto done;
        }

        snap.flags &= ~0x80U;
        fan_feedforward_pending = true;
    }

    if (fan_feedforward_is_needed(snap.target_fan_rpm))
    {
        if (fan_apply_feedforward(snap.target_fan_rpm, now))
            fan_set_ctrl_state(APP_AUTO_FAN_CTRL_ADJUSTING);
        goto done;
    }

    if (!tach_ok)
    {
        fan_reset_error_confirmation();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_WAIT_TACH);
        last_fan_adj_tick = now;
        goto done;
    }

    error = (int32_t)snap.target_fan_rpm - (int32_t)fan_snap.rpm;
    snap.fan_error_rpm = (int16_t)error;

    if ((uint32_t)(now - last_fan_adj_tick) < APP_AUTO_FAN_CONTROL_PERIOD_MS)
        goto done;

    /* Every decision, including a confirmation-only cycle, advances cadence. */
    last_fan_adj_tick = now;
    absolute_error = fan_abs_error_rpm(error);
    was_in_tolerance = (fan_ctrl_state == APP_AUTO_FAN_CTRL_IN_TOLERANCE);

    if (AppAutoFan_ErrorIsInTolerance(absolute_error, was_in_tolerance))
    {
        fan_reset_error_confirmation();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_IN_TOLERANCE);
        goto done;
    }

    direction = (error > 0) ? 1 : -1;
    fan_set_ctrl_state(APP_AUTO_FAN_CTRL_ADJUSTING);

    if (!fan_error_direction_is_confirmed(direction, absolute_error))
        goto done;

    trim_step = AppAutoFan_SelectTrimStepX100(absolute_error);
    commanded_duty = fan_snap.target_duty_x100;

    if (direction > 0)
    {
        if (commanded_duty >= APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        {
            fan_set_ctrl_state(APP_AUTO_FAN_CTRL_SATURATED_HIGH);
            goto done;
        }

        new_duty = (uint16_t)(commanded_duty + trim_step);
        if (new_duty < commanded_duty ||
            new_duty > APP_AUTO_FAN_NORMAL_MAX_DUTY_X100)
        {
            new_duty = APP_AUTO_FAN_NORMAL_MAX_DUTY_X100;
        }
    }
    else
    {
        if (commanded_duty <= APP_AUTO_FAN_NORMAL_MIN_DUTY_X100)
        {
            fan_set_ctrl_state(APP_AUTO_FAN_CTRL_SATURATED_LOW);
            goto done;
        }

        if (commanded_duty <= APP_AUTO_FAN_NORMAL_MIN_DUTY_X100 + trim_step)
            new_duty = APP_AUTO_FAN_NORMAL_MIN_DUTY_X100;
        else
            new_duty = (uint16_t)(commanded_duty - trim_step);
    }

    if (new_duty != commanded_duty && fan_set_normal_duty(new_duty))
    {
        if (fan_adj_count != UINT32_MAX)
            fan_adj_count++;
    }

done:
    fan_publish_control_diag();
}

/**
 * @brief AUTO 风门子状态机：等待位置有效和空闲，使用死区避免频繁动作，并在温度故障时执行安全开度。
 * @note 文件内部辅助函数，不属于对外 API。
 */
__attribute__((unused))
static void damper_control_process(void)
{
    DamperSnapshot damper_snap;
    uint32_t now;
    bool pos_valid;
    int32_t damper_state_en;
    int32_t target;

    if (!AppDamper_GetSnapshot(&damper_snap)) return;

    now = HAL_GetTick();
    pos_valid = (damper_snap.flags & 0x01U) != 0U;
    damper_state_en = (int32_t)damper_snap.damper_state;
    target = snap.target_damper_steps;

    snap.damper_state           = damper_snap.damper_state;
    snap.damper_position_valid  = pos_valid ? 1U : 0U;
    snap.actual_damper_steps    = damper_snap.current_steps;
    snap.requested_damper_target_steps = target;

    if (!pos_valid)
    {
        auto_damper_owned = false;
        damper_pos_invalid = true;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_POSITION;
        return;
    }

    damper_pos_invalid = false;

    /* FAULT state */
    if (damper_state_en == DAMPER_STATE_FAULT)
    {
        auto_damper_owned = false;
        failsafe_stop_issued = false;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAULT;
        damper_fault_count++;
        return;
    }

    /* Moving or POST_MOVE_HOLD */
    if (damper_state_en == DAMPER_STATE_MOVING_FORWARD ||
        damper_state_en == DAMPER_STATE_MOVING_REVERSE)
    {
        if (auto_damper_owned)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_MOVING;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        }
        return;
    }

    if (damper_state_en == DAMPER_STATE_POST_MOVE_HOLD)
    {
        if (auto_damper_owned)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_POST_HOLD;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        }
        return;
    }

    /* Movement finished */
    if (auto_damper_owned &&
        (damper_state_en == DAMPER_STATE_IDLE_RELEASED ||
         damper_state_en == DAMPER_STATE_STOPPED))
    {
        auto_damper_owned = false;
    }

    /* Failsafe: temp fault → target = 0 */
    if ((snap.flags & 0x10U) != 0U)
    {
        if (!auto_damper_owned)
        {
            if (failsafe_stop_issued)
            {
                if (damper_state_en == DAMPER_STATE_STOPPED ||
                    damper_state_en == DAMPER_STATE_IDLE_RELEASED)
                {
                    failsafe_stop_issued = false;
                }
                else
                {
                    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAILSAFE_STOPPING;
                    return;
                }
            }

            {
                int64_t diff64 = 0LL - (int64_t)damper_snap.current_steps;
                uint32_t ad = (diff64 < 0) ? (uint32_t)(-diff64) : (uint32_t)diff64;

                if (ad < APP_AUTO_DAMPER_DEADBAND_STEPS)
                {
                    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_IN_DEADBAND;
                    return;
                }
            }

            if (target != 0)
            {
                damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
                return;
            }
        }
        else
        {
            if (target != 0)
            {
                AppDamper_Stop();
                auto_damper_owned = false;
                failsafe_stop_issued = true;
                damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAILSAFE_STOPPING;
                return;
            }
        }
    }
    else
    {
        failsafe_stop_issued = false;
    }

    /* Not idle */
    if (damper_state_en != DAMPER_STATE_IDLE_RELEASED &&
        damper_state_en != DAMPER_STATE_STOPPED)
    {
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
        return;
    }

    /* Idle - evaluate deadband */
    if ((uint32_t)(now - last_damper_act_tick) < APP_AUTO_DAMPER_CONTROL_PERIOD_MS)
        return;

    {
        int64_t diff64 = (int64_t)target - (int64_t)damper_snap.current_steps;
        uint32_t ad = (diff64 < 0) ? (uint32_t)(-diff64) : (uint32_t)diff64;

        if (ad < APP_AUTO_DAMPER_DEADBAND_STEPS)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_IN_DEADBAND;
            return;
        }
    }

    if (target < 0)
        target = 0;
    if (target > (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS)
        target = (int32_t)APP_DAMPER_FULL_TRAVEL_STEPS;

    snap.last_commanded_damper_steps = target;
    snap.damper_error_steps = target - damper_snap.current_steps;

    {
        uint8_t result = AppDamper_MoveAbsolute(target);
        snap.damper_control_result = result;

        if (result == DAMPER_STATUS_OK)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_COMMAND_SUBMITTED;
            auto_damper_owned = true;
            damper_cmd_count++;
            last_damper_act_tick = now;
        }
        else if (result == DAMPER_STATUS_BUSY)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_IDLE;
            damper_busy_count++;
            last_damper_act_tick = now;
        }
        else if (result == DAMPER_STATUS_NO_VALID_DATA)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_WAIT_POSITION;
            auto_damper_owned = false;
        }
        else if (result == DAMPER_STATUS_PARAM_RANGE)
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_COMMAND_ERROR;
        }
        else
        {
            damper_ctrl_state = APP_AUTO_DAMPER_CTRL_FAULT;
            damper_fault_count++;
            auto_damper_owned = false;
        }
    }

    snap.damper_command_count = damper_cmd_count;
    snap.damper_busy_count    = damper_busy_count;
    snap.damper_fault_count   = damper_fault_count;
    snap.last_damper_control_tick = last_damper_act_tick;
    snap.damper_auto_owned    = auto_damper_owned ? 1U : 0U;
    snap.damper_failsafe_stop_issued = failsafe_stop_issued ? 1U : 0U;
    snap.damper_control_state = (uint8_t)damper_ctrl_state;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppAutoControl_Init(void)
{
#if APP_AUTO_CONTROL_ENABLED
    mode = APP_AUTO_MODE_MANUAL;

    if (!config_check())
    {
        state  = APP_AUTO_STATE_CONFIG_ERROR;
        init_ok = false;
        return;
    }

    state = APP_AUTO_STATE_UNINITIALIZED;
    init_ok = true;

    fan_ctrl_state    = APP_AUTO_FAN_CTRL_INACTIVE;
    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
#else
    mode   = APP_AUTO_MODE_MANUAL;
    state  = APP_AUTO_STATE_UNAVAILABLE;
    init_ok = false;

    fan_ctrl_state    = APP_AUTO_FAN_CTRL_UNAVAILABLE;
    damper_ctrl_state = APP_AUTO_DAMPER_CTRL_UNAVAILABLE;
#endif

    auto_fan_duty              = 0U;
    fan_feedforward_target_rpm = 0U;
    fan_error_confirm_direction = 0;
    fan_error_confirm_count    = 0U;
    fan_feedforward_pending    = false;
    last_fan_adj_tick          = 0U;
    fan_adj_count              = 0U;
    fan_fault_count            = 0U;

    damper_ctrl_state      = APP_AUTO_DAMPER_CTRL_INACTIVE;
    last_damper_act_tick   = 0U;
    damper_cmd_count       = 0U;
    damper_busy_count      = 0U;
    damper_fault_count     = 0U;
    auto_damper_owned      = false;
    failsafe_stop_issued   = false;
    damper_pos_invalid     = false;

    snap.mode                  = (uint8_t)mode;
    snap.state                 = (uint8_t)state;
    snap.flags                 = 0x00U;
    snap.ntc_state             = 0U;
    snap.ntc_range_status      = 0U;
    snap.measured_temp_centi_c = 0;
    snap.control_temp_centi_c  = 0;
    snap.target_fan_rpm        = 0U;
    snap.target_damper_steps   = 0;
    snap.update_seq            = 0U;
    snap.last_update_tick      = 0U;

    snap.fan_control_state    = (uint8_t)fan_ctrl_state;
    snap.fan_state            = 0U;
    snap.fan_tach_valid       = 0U;
    snap.fan_control_result   = 0U;
    snap.actual_fan_rpm       = 0U;
    snap.applied_fan_duty_x100 = 0U;
    snap.fan_error_rpm        = 0;
    snap.last_fan_control_tick = 0U;
    snap.fan_adjust_count     = 0U;
    snap.fan_fault_count      = 0U;

    snap.damper_control_state        = (uint8_t)damper_ctrl_state;
    snap.damper_state                = 0U;
    snap.damper_position_valid       = 0U;
    snap.damper_control_result       = 0U;
    snap.damper_auto_owned           = 0U;
    snap.damper_failsafe_stop_issued = 0U;
    snap.reserved_damper             = 0U;
    snap.actual_damper_steps         = 0;
    snap.requested_damper_target_steps = 0;
    snap.last_commanded_damper_steps = 0;
    snap.damper_error_steps          = 0;
    snap.last_damper_control_tick    = 0U;
    snap.damper_command_count        = 0U;
    snap.damper_busy_count           = 0U;
    snap.damper_fault_count          = 0U;

    last_process_tick = 0U;
    auto_start_tick   = 0U;
    update_seq        = 0U;
}

/**
 * @brief 切换模块工作模式并完成必要的状态复位。
 * @param new_mode 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
uint8_t AppAutoControl_SetMode(uint8_t new_mode)
{
#if APP_AUTO_CONTROL_ENABLED
    bool restart_was_inhibited = false;

    if (!init_ok)
        return APP_AUTO_SET_MODE_CONFIG_ERROR;

    if (state == APP_AUTO_STATE_CONFIG_ERROR)
        return APP_AUTO_SET_MODE_CONFIG_ERROR;

    if (new_mode != APP_AUTO_MODE_MANUAL && new_mode != APP_AUTO_MODE_AUTO)
        return APP_AUTO_SET_MODE_INVALID_PARAM;

    if (new_mode == APP_AUTO_MODE_AUTO)
    {
        if (AppFanHealth_IsFaultLatched())
            return APP_AUTO_SET_MODE_CONFIG_ERROR;

        restart_was_inhibited = AppFanHealth_RestartIsInhibited();
        if (restart_was_inhibited && !AppFanHealth_AuthorizeRestart())
            return APP_AUTO_SET_MODE_CONFIG_ERROR;
    }

    if ((AppAutoMode)new_mode == mode)
    {
        if (new_mode == APP_AUTO_MODE_AUTO && restart_was_inhibited)
        {
            fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
            fan_feedforward_target_rpm = 0U;
            fan_feedforward_pending = true;
            fan_reset_error_confirmation();
            last_fan_adj_tick = 0U;
            auto_start_tick = HAL_GetTick();
        }
        return APP_AUTO_SET_MODE_OK;
    }

    if (new_mode == APP_AUTO_MODE_MANUAL && mode == APP_AUTO_MODE_AUTO
        && APP_AUTO_DAMPER_STOP_ON_MANUAL && auto_damper_owned)
    {
        AppDamper_Stop();
        auto_damper_owned = false;
        failsafe_stop_issued = false;
    }

    mode = (AppAutoMode)new_mode;
    snap.mode = (uint8_t)mode;

    if (mode == APP_AUTO_MODE_AUTO)
    {
        auto_start_tick = HAL_GetTick();
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
        fan_feedforward_target_rpm = 0U;
        fan_feedforward_pending = true;
        fan_reset_error_confirmation();
        last_fan_adj_tick = 0U;
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        auto_damper_owned = false;
        failsafe_stop_issued = false;
        damper_pos_invalid = false;
        do_evaluation();
    }
    else
    {
        state = APP_AUTO_STATE_MANUAL;
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
        fan_feedforward_pending = false;
        fan_feedforward_target_rpm = 0U;
        fan_reset_error_confirmation();
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        last_process_tick = 0U;
        snap.state = (uint8_t)state;
    }

    return APP_AUTO_SET_MODE_OK;
#else
    (void)new_mode;
    return APP_AUTO_SET_MODE_UNAVAILABLE;
#endif
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppAutoControl_Process(void)
{
    if (!init_ok) return;

#if APP_AUTO_CONTROL_ENABLED
    if (state == APP_AUTO_STATE_UNINITIALIZED)
    {
        if (mode == APP_AUTO_MODE_AUTO)
        {
            auto_start_tick = HAL_GetTick();
            do_evaluation();
        }
        else
        {
            state = APP_AUTO_STATE_MANUAL;
            snap.state = (uint8_t)state;
        }
        return;
    }

    if (mode == APP_AUTO_MODE_MANUAL)
    {
        state = APP_AUTO_STATE_MANUAL;
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
        fan_feedforward_pending = false;
        fan_reset_error_confirmation();
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
        snap.fan_control_state = (uint8_t)fan_ctrl_state;
        snap.damper_control_state = (uint8_t)damper_ctrl_state;
        snap.state = (uint8_t)state;
        return;
    }

    {
        uint32_t now = HAL_GetTick();

        if ((uint32_t)(now - last_process_tick) >= APP_AUTO_CONTROL_PERIOD_MS)
        {
            last_process_tick = now;
            do_evaluation();
        }
    }

    if (targets_are_valid())
    {
        fan_control_process();
        damper_control_process();
    }
    else
    {
        fan_set_ctrl_state(APP_AUTO_FAN_CTRL_INACTIVE);
        fan_reset_error_confirmation();
        damper_ctrl_state = APP_AUTO_DAMPER_CTRL_INACTIVE;
    }

    snap.fan_control_state = (uint8_t)fan_ctrl_state;
    snap.damper_control_state = (uint8_t)damper_ctrl_state;
    snap.state = (uint8_t)state;
#endif
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param s 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppAutoControl_GetSnapshot(AppAutoControlSnapshot *s)
{
    if (s == NULL) return false;
    *s = snap;
    return true;
}

/**
 * @brief 返回当前工作模式。
 * @return 返回值含义见函数名、对应枚举或调用点。
 */
uint8_t AppAutoControl_GetMode(void)
{
    return (uint8_t)mode;
}
