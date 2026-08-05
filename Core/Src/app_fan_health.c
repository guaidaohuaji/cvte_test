/**
 * @file app_fan_health.c
 * @brief 风机异常判定、故障锁存与停机（实现文件）。
 *
 * 模块职责：根据实际 PWM 查表得到预期 RPM，检测持续转速偏差或测速丢失，经过 settling 和 5 秒确认后锁存并停机。
 * 数据输入：AppFan 快照、标定表预期 RPM、控制模式和时间。
 * 数据输出：健康状态/故障现场快照；对 AppFan 的安全停机、清锁存和重启授权。
 * 执行上下文：每轮主循环执行；故障判定完全在 MCU 侧完成，上位机只查询和清除。
 * 阅读重点：按 settling→suspect→confirm→latch 四阶段阅读，注意 600/450 RPM 迟滞和清故障不等于授权重启。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_fan_health.h"

#include "app_auto_fan_profile.h"
#include "app_fan.h"
#include "app_fan_health_config.h"
#include "stm32f4xx_hal.h"

#include <limits.h>
#include <stddef.h>

#if APP_FAN_HEALTH_MONITOR_ENABLE

/* snap 保存当前判定状态；fault_* 现场在锁存后保持不变，直到显式
 * 清故障，便于上位机解释停机原因。 */
static AppFanHealthSnapshot snap;
static bool was_enabled;
static bool was_boosting;
static bool reference_duty_valid;
static uint16_t last_reference_duty_x100;
static uint32_t settling_start_tick;
static uint32_t settling_duration_ms;
static bool suspect_active;
static AppFanHealthState suspect_state;
static int8_t suspect_direction;
static uint32_t suspect_start_tick;

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t increment_saturated(uint32_t value)
{
    return (value == UINT32_MAX) ? UINT32_MAX : value + 1U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param value 写入值，读取操作时通常为 0。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t abs_i32_to_u16(int32_t value)
{
    uint32_t magnitude = (value < 0) ? (uint32_t)(-value) : (uint32_t)value;
    return (magnitude > UINT16_MAX) ? UINT16_MAX : (uint16_t)magnitude;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param now 当前 HAL 毫秒 tick。
 * @param start 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t elapsed_ms(uint32_t now, uint32_t start)
{
    return now - start;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static AppFanHealthState latched_state(void)
{
    return (snap.fault_type == APP_FAN_HEALTH_FAULT_TACH_LOST)
         ? APP_FAN_HEALTH_STATE_TACH_FAULT_LATCHED
         : APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED;
}

/**
 * @brief 清除指定诊断或锁存字段。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void clear_suspect(void)
{
    suspect_active = false;
    suspect_state = APP_FAN_HEALTH_STATE_NORMAL;
    suspect_direction = 0;
    suspect_start_tick = 0U;
    snap.abnormal_elapsed_ms = 0U;
}

/**
 * @brief 清除指定诊断或锁存字段。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void clear_measurement_fields(void)
{
    snap.monitoring_active = 0U;
    snap.tach_valid = 0U;
    snap.applied_duty_x100 = 0U;
    snap.reference_duty_x100 = 0U;
    snap.expected_rpm = 0U;
    snap.actual_rpm = 0U;
    snap.deviation_rpm = 0;
    snap.absolute_deviation_rpm = 0U;
    snap.settling_remaining_ms = 0U;
}

/**
 * @brief 清除内部临时状态，使后续流程重新同步。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void reset_runtime_tracking(void)
{
    was_enabled = false;
    was_boosting = false;
    reference_duty_valid = false;
    last_reference_duty_x100 = 0U;
    settling_start_tick = 0U;
    settling_duration_ms = 0U;
    clear_suspect();
}

/**
 * @brief 清除内部临时状态，使后续流程重新同步。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void reset_for_fan_off(void)
{
    uint32_t reset_count = increment_saturated(snap.reset_count);
    uint32_t update_count = snap.update_count;
    uint32_t suspect_count = snap.suspect_count;
    uint32_t fault_count = snap.fault_count;
    uint32_t shutdown_count = snap.shutdown_count;
    uint32_t clear_count = snap.clear_count;
    uint8_t restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;

    snap = (AppFanHealthSnapshot){0};
    snap.state = APP_FAN_HEALTH_STATE_DISABLED;
    snap.reset_count = reset_count;
    snap.update_count = update_count;
    snap.suspect_count = suspect_count;
    snap.fault_count = fault_count;
    snap.shutdown_count = shutdown_count;
    snap.clear_count = clear_count;
    snap.restart_inhibited = restart_inhibited;

    reset_runtime_tracking();
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param duty_x100 占空比，单位 0.01%，例如 100 表示 1%。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool duty_is_in_profile(uint16_t duty_x100)
{
    return duty_x100 >= APP_FAN_HEALTH_MIN_DUTY_X100 &&
           duty_x100 <= APP_FAN_HEALTH_MAX_DUTY_X100;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param a 见调用点；该参数只在本次调用期间有效。
 * @param b 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t duty_delta(uint16_t a, uint16_t b)
{
    return (a >= b) ? (uint16_t)(a - b) : (uint16_t)(b - a);
}

/**
 * @brief 进入新的状态阶段并记录起始条件。
 * @param now 当前 HAL 毫秒 tick。
 * @param duration_ms 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void begin_settling(uint32_t now, uint32_t duration_ms)
{
    settling_start_tick = now;
    settling_duration_ms = duration_ms;
    snap.state = APP_FAN_HEALTH_STATE_SETTLING;
    snap.settling_remaining_ms = duration_ms;
    clear_suspect();
}

/**
 * @brief 判断启动加力后、占空比大步变化后等暂态等待窗口是否仍有效，暂态内不做偏差故障判定。
 * @param now 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool settling_is_active(uint32_t now)
{
    uint32_t elapsed;

    if (settling_duration_ms == 0U)
        return false;

    elapsed = elapsed_ms(now, settling_start_tick);
    if (elapsed >= settling_duration_ms)
    {
        settling_duration_ms = 0U;
        snap.settling_remaining_ms = 0U;
        return false;
    }

    snap.settling_remaining_ms = settling_duration_ms - elapsed;
    return true;
}

/**
 * @brief 记录异常确认起点和本次诊断参考值，之后只有异常连续维持满 5 秒才会正式锁存。
 * @param state 见调用点；该参数只在本次调用期间有效。
 * @param direction 见调用点；该参数只在本次调用期间有效。
 * @param now 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void begin_suspect(AppFanHealthState state, int8_t direction, uint32_t now)
{
    if (!suspect_active || suspect_state != state ||
        suspect_direction != direction)
    {
        suspect_active = true;
        suspect_state = state;
        suspect_direction = direction;
        suspect_start_tick = now;
        snap.abnormal_elapsed_ms = 0U;
        snap.suspect_count = increment_saturated(snap.suspect_count);
    }

    snap.state = state;
}

/**
 * @brief 把故障类型和现场数据锁存，并调用 AppFan_TripSafetyFault() 强制风机 0%且禁止自动重启。
 * @param fault_type 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void latch_fault(AppFanHealthFaultType fault_type)
{
    bool shutdown_ok;

    if (snap.fault_latched != 0U)
        return;

    snap.fault_latched = 1U;
    snap.fault_type = (uint8_t)fault_type;
    snap.state = (fault_type == APP_FAN_HEALTH_FAULT_TACH_LOST)
               ? APP_FAN_HEALTH_STATE_TACH_FAULT_LATCHED
               : APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED;
    snap.monitoring_active = 0U;

    snap.fault_applied_duty_x100 = snap.applied_duty_x100;
    snap.fault_expected_rpm = snap.expected_rpm;
    snap.fault_actual_rpm = snap.actual_rpm;
    snap.fault_deviation_rpm = snap.deviation_rpm;
    snap.fault_count = increment_saturated(snap.fault_count);

    shutdown_ok = AppFan_TripSafetyFault();
    snap.shutdown_succeeded = shutdown_ok ? 1U : 0U;
    snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;
    snap.shutdown_count = increment_saturated(snap.shutdown_count);
}

/**
 * @brief 根据最新输入更新内部状态。
 * @param now 当前 HAL 毫秒 tick。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void update_suspect_elapsed(uint32_t now)
{
    uint32_t elapsed = elapsed_ms(now, suspect_start_tick);
    snap.abnormal_elapsed_ms = elapsed;

    if (elapsed >= APP_FAN_HEALTH_CONFIRM_MS)
    {
        if (suspect_state == APP_FAN_HEALTH_STATE_TACH_SUSPECT)
        {
            latch_fault(APP_FAN_HEALTH_FAULT_TACH_LOST);
        }
        else if (snap.deviation_rpm < 0)
        {
            latch_fault(APP_FAN_HEALTH_FAULT_SPEED_LOW);
        }
        else
        {
            latch_fault(APP_FAN_HEALTH_FAULT_SPEED_HIGH);
        }
    }
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppFanHealth_Init(void)
{
    snap = (AppFanHealthSnapshot){0};
    snap.state = APP_FAN_HEALTH_STATE_DISABLED;
    snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;
    reset_runtime_tracking();
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppFanHealth_Process(void)
{
    AppFanSnapshot fan;
    uint32_t now;
    uint16_t reference_duty;
    bool reference_available;

    snap.update_count = increment_saturated(snap.update_count);

    if (!AppFan_GetSnapshot(&fan))
    {
        if (snap.fault_latched == 0U)
        {
            snap.state = APP_FAN_HEALTH_STATE_DISABLED;
            clear_measurement_fields();
        }
        return;
    }

    now = HAL_GetTick();

    if (snap.fault_latched != 0U)
    {
        snap.state = latched_state();
        snap.monitoring_active = 0U;
        snap.applied_duty_x100 = fan.applied_duty_x100;
        snap.tach_valid = fan.tach_valid ? 1U : 0U;
        snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;
        if (!fan.enabled && fan.applied_duty_x100 == 0U)
            snap.shutdown_succeeded = 1U;
        return;
    }

    snap.applied_duty_x100 = fan.applied_duty_x100;
    snap.actual_rpm = fan.rpm;
    snap.tach_valid = fan.tach_valid ? 1U : 0U;
    snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;

    if (!fan.enabled || fan.applied_duty_x100 == 0U)
    {
        if (was_enabled || snap.state != APP_FAN_HEALTH_STATE_DISABLED)
            reset_for_fan_off();
        return;
    }

    was_enabled = true;

    if (fan.state == APP_FAN_STATE_STARTUP_BOOST)
    {
        was_boosting = true;
        reference_duty_valid = false;
        settling_duration_ms = 0U;
        clear_suspect();
        snap.state = APP_FAN_HEALTH_STATE_STARTUP_BOOST;
        snap.monitoring_active = 0U;
        snap.reference_duty_x100 = 0U;
        snap.expected_rpm = 0U;
        snap.deviation_rpm = 0;
        snap.absolute_deviation_rpm = 0U;
        snap.settling_remaining_ms = 0U;
        return;
    }

    reference_available = false;
    reference_duty = 0U;

    if (duty_is_in_profile(fan.applied_duty_x100))
    {
        reference_duty = fan.applied_duty_x100;
        reference_available = true;

        if (was_boosting)
        {
            was_boosting = false;
            begin_settling(now, APP_FAN_HEALTH_POST_BOOST_SETTLE_MS);
        }
        else if (!reference_duty_valid)
        {
            begin_settling(now, APP_FAN_HEALTH_DUTY_CHANGE_SETTLE_MS);
        }
        else if (duty_delta(reference_duty, last_reference_duty_x100) >=
                 APP_FAN_HEALTH_LARGE_DUTY_STEP_X100)
        {
            begin_settling(now, APP_FAN_HEALTH_DUTY_CHANGE_SETTLE_MS);
        }

        last_reference_duty_x100 = reference_duty;
        reference_duty_valid = true;
    }
    else if (reference_duty_valid &&
             (fan.state == APP_FAN_STATE_NO_TACH ||
              (fan.applied_duty_x100 == APP_AUTO_FAN_FAILSAFE_DUTY_X100 &&
               !fan.tach_valid)))
    {
        /* Existing controllers may switch to 100% when tach is lost. Keep
         * the last normal 10..80% duty as the diagnostic reference. */
        reference_duty = last_reference_duty_x100;
        reference_available = true;
    }

    if (!reference_available)
    {
        clear_suspect();
        snap.state = APP_FAN_HEALTH_STATE_DISABLED;
        snap.monitoring_active = 0U;
        snap.reference_duty_x100 = 0U;
        snap.expected_rpm = 0U;
        snap.deviation_rpm = 0;
        snap.absolute_deviation_rpm = 0U;
        snap.settling_remaining_ms = 0U;
        return;
    }

    snap.reference_duty_x100 = reference_duty;
    snap.expected_rpm = AppAutoFan_EstimateRpmFromDutyX100(reference_duty);
    snap.monitoring_active = 1U;

    if (settling_is_active(now))
    {
        snap.state = APP_FAN_HEALTH_STATE_SETTLING;
        snap.deviation_rpm = 0;
        snap.absolute_deviation_rpm = 0U;
        return;
    }

    if (!fan.tach_valid || fan.state == APP_FAN_STATE_NO_TACH)
    {
        snap.actual_rpm = 0U;
        snap.deviation_rpm = -(int16_t)snap.expected_rpm;
        snap.absolute_deviation_rpm = snap.expected_rpm;
        begin_suspect(APP_FAN_HEALTH_STATE_TACH_SUSPECT, 0, now);
        update_suspect_elapsed(now);
        return;
    }

    {
        int32_t deviation = (int32_t)fan.rpm - (int32_t)snap.expected_rpm;
        uint16_t absolute_deviation = abs_i32_to_u16(deviation);
        int8_t deviation_direction = (deviation < 0) ? -1 : 1;

        if (suspect_active &&
            suspect_state == APP_FAN_HEALTH_STATE_TACH_SUSPECT)
        {
            clear_suspect();
        }

        snap.deviation_rpm = (deviation > INT16_MAX) ? INT16_MAX
                           : (deviation < INT16_MIN) ? INT16_MIN
                           : (int16_t)deviation;
        snap.absolute_deviation_rpm = absolute_deviation;
        if (absolute_deviation > snap.maximum_absolute_deviation_rpm)
            snap.maximum_absolute_deviation_rpm = absolute_deviation;

        if (absolute_deviation > APP_FAN_HEALTH_DEVIATION_ENTER_RPM)
        {
            begin_suspect(APP_FAN_HEALTH_STATE_SPEED_SUSPECT,
                          deviation_direction, now);
            update_suspect_elapsed(now);
            return;
        }

        if (absolute_deviation <= APP_FAN_HEALTH_DEVIATION_CLEAR_RPM)
        {
            clear_suspect();
            snap.state = APP_FAN_HEALTH_STATE_NORMAL;
            return;
        }

        if (suspect_active &&
            suspect_state == APP_FAN_HEALTH_STATE_SPEED_SUSPECT)
        {
            snap.state = APP_FAN_HEALTH_STATE_SPEED_SUSPECT;
            update_suspect_elapsed(now);
        }
        else
        {
            snap.state = APP_FAN_HEALTH_STATE_NORMAL;
        }
    }
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_GetSnapshot(AppFanHealthSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = snap;
    return true;
}

/**
 * @brief 清除锁存或历史状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_ClearFault(void)
{
    uint32_t update_count;
    uint32_t suspect_count;
    uint32_t fault_count;
    uint32_t shutdown_count;
    uint32_t clear_count;
    uint32_t reset_count;

    if (snap.fault_latched == 0U)
        return true;

    if (!AppFan_ClearSafetyFault())
        return false;

    update_count = snap.update_count;
    suspect_count = snap.suspect_count;
    fault_count = snap.fault_count;
    shutdown_count = snap.shutdown_count;
    clear_count = increment_saturated(snap.clear_count);
    reset_count = snap.reset_count;

    snap = (AppFanHealthSnapshot){0};
    snap.state = APP_FAN_HEALTH_STATE_DISABLED;
    snap.update_count = update_count;
    snap.suspect_count = suspect_count;
    snap.fault_count = fault_count;
    snap.shutdown_count = shutdown_count;
    snap.clear_count = clear_count;
    snap.reset_count = reset_count;
    snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;
    reset_runtime_tracking();
    return true;
}

/**
 * @brief 在显式操作员命令后解除重启禁止。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_AuthorizeRestart(void)
{
    bool result;

    if (snap.fault_latched != 0U)
        return false;

    result = AppFan_AuthorizeRestart();
    snap.restart_inhibited = AppFan_IsRestartInhibited() ? 1U : 0U;
    return result;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_IsFaultLatched(void)
{
    return snap.fault_latched != 0U;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_RestartIsInhibited(void)
{
    return AppFan_IsRestartInhibited();
}

#else

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 */
void AppFanHealth_Init(void)
{
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppFanHealth_Process(void)
{
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param snapshot 输出快照指针，成功时写入当前一致性副本。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_GetSnapshot(AppFanHealthSnapshot *snapshot)
{
    if (snapshot == NULL)
        return false;
    *snapshot = (AppFanHealthSnapshot){0};
    snapshot->state = APP_FAN_HEALTH_STATE_DISABLED;
    return true;
}

/**
 * @brief 清除锁存或历史状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_ClearFault(void)
{
    return true;
}

/**
 * @brief 在显式操作员命令后解除重启禁止。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_AuthorizeRestart(void)
{
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_IsFaultLatched(void)
{
    return false;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFanHealth_RestartIsInhibited(void)
{
    return false;
}

#endif
