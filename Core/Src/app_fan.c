/**
 * @file app_fan.c
 * @brief 风机 PWM 执行层与启动状态机（实现文件）。
 *
 * 模块职责：管理 TIM10 风机 PWM、5 秒启动加力、反馈有效性、无测速状态以及健康模块使用的安全锁。
 * 数据输入：手动/AUTO 控制模块给出的启停和占空比；风机反馈模块给出的频率。
 * 数据输出：PB8/TIM10_CH1 PWM；风机状态、占空比、FG 和 RPM 快照。
 * 执行上下文：所有控制写入最终都汇聚到本模块；健康故障可立即强制 0%，清故障后仍需显式授权重启。
 * 阅读重点：先读 AppFan_SetEnabled()，再读 AppFan_Process() 中 boost、反馈和状态更新，最后看安全锁 API。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_fan.h"
#include "app_fan_config.h"
#include "app_fan_feedback_adc.h"
#include "tim.h"
#include <stddef.h>

#define APP_FAN_TACH_REACQUIRE_GRACE_MS  400U
#define APP_FAN_PERSISTENT_NO_TACH_MS   1000U

static AppFanState state;
static bool     enabled;
static uint16_t target_duty;
static uint16_t applied_duty;
static uint32_t boost_start_tick;
static uint32_t last_fg_fmhz;
static uint16_t last_rpm;
static uint32_t last_tach_tick;
static uint32_t last_fg_update_seq;
static uint32_t fan_start_tick;
static bool     init_ok;
static bool     tach_valid;
static bool     safety_fault_latched;
static bool     restart_inhibited;
static bool     ever_had_valid_tach;
static uint32_t last_duty_change_tick;

/**
 * @brief 把 0.01%单位占空比换算为 TIM10 CCR，并更新 applied_duty；所有上层风机命令最终经过此处。
 * @param d 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool apply_duty(uint16_t d)
{
    uint64_t cmp = ((uint64_t)(APP_FAN_PWM_PERIOD + 1U) * (uint64_t)d + 5000ULL) / 10000ULL;
    if (cmp > 65535ULL) cmp = 65535ULL;
    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, (uint32_t)cmp);
    uint32_t rd = htim10.Instance->CCR1;
    return (rd == (uint32_t)cmp);
}

/**
 * @brief 执行内部数值计算并进行边界检查。
 * @param fmhz 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t calc_rpm(uint32_t fmhz)
{
    uint64_t r = ((uint64_t)fmhz * APP_FAN_RPM_FACTOR + 500ULL) / 1000ULL;
    return (r > 65535ULL) ? 65535U : (uint16_t)r;
}

/**
 * @brief 判断指定条件是否成立。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool is_grace_period_active(void)
{
    return (HAL_GetTick() - fan_start_tick) < APP_FAN_STARTUP_BOOST_MS;
}

/**
 * @brief 判断指定条件是否成立。
 * @param now 当前 HAL 毫秒 tick。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool is_reacquire_grace_active(uint32_t now)
{
    return ((now - last_duty_change_tick) < APP_FAN_TACH_REACQUIRE_GRACE_MS);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void sync_feedback_update_sequence(void)
{
    AppFanFeedbackSnapshot tmp;

    if (AppFanFeedback_GetSnapshot(&tmp))
        last_fg_update_seq = tmp.update_seq;
    else
        last_fg_update_seq = 0U;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_Init(void)
{
    state       = APP_FAN_STATE_OFF;
    enabled     = false;
    target_duty = APP_FAN_MAX_DUTY_X100;
    applied_duty = 0U;
    last_fg_fmhz = 0U;
    last_rpm     = 0U;
    last_tach_tick = 0U;
    last_fg_update_seq = 0U;
    fan_start_tick     = 0U;
    last_duty_change_tick = 0U;
    init_ok = false;
    tach_valid = false;
    ever_had_valid_tach = false;
    safety_fault_latched = false;
    restart_inhibited = false;

    if ((APP_FAN_PWM_PRESCALER > 65535U) ||
        (APP_FAN_PWM_PERIOD > 65535U) ||
        (APP_FAN_PWM_PERIOD == 0U) ||
        (APP_FAN_MIN_DUTY_X100 > APP_FAN_MAX_DUTY_X100))
    {
        state = APP_FAN_STATE_CONFIG_ERROR;
        return false;
    }

    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, 0U);

    if (HAL_TIM_PWM_Start(&htim10, TIM_CHANNEL_1) != HAL_OK)
    {
        state = APP_FAN_STATE_PWM_ERROR;
        return false;
    }

    __HAL_TIM_SET_COMPARE(&htim10, TIM_CHANNEL_1, 0U);
    init_ok = true;
    return true;
}

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void handle_disable(void)
{
    if (!apply_duty(0U)) { state = APP_FAN_STATE_PWM_ERROR; enabled = false; return; }
    applied_duty = 0U;
    enabled     = false;
    state       = safety_fault_latched
                ? APP_FAN_STATE_SAFETY_LOCKED
                : APP_FAN_STATE_OFF;
    last_fg_fmhz = 0U;
    last_rpm     = 0U;
    last_tach_tick = 0U;
    tach_valid = false;
    ever_had_valid_tach = false;
}

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param duty 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool handle_fresh_enable(uint16_t duty)
{
    if (!apply_duty(APP_FAN_MAX_DUTY_X100))
    {
        apply_duty(0U);
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }

    AppFanFeedback_ResetMeasurement();
    sync_feedback_update_sequence();

    applied_duty     = APP_FAN_MAX_DUTY_X100;
    enabled          = true;
    boost_start_tick = HAL_GetTick();
    state            = APP_FAN_STATE_STARTUP_BOOST;
    fan_start_tick   = HAL_GetTick();
    last_fg_fmhz     = 0U;
    last_rpm         = 0U;
    last_tach_tick   = 0U;
    tach_valid       = false;
    ever_had_valid_tach = false;
    last_duty_change_tick = HAL_GetTick();

    target_duty = duty;
    return true;
}

/**
 * @brief 处理一个已分类的事件并推进状态机。
 * @param duty 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool handle_duty_update(uint16_t duty)
{
    if (target_duty == duty) return true;

    target_duty = duty;

    if (state == APP_FAN_STATE_STARTUP_BOOST)
        return true;

    if (!apply_duty(duty))
    {
        apply_duty(0U);
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }
    applied_duty = duty;
    AppFanFeedback_ReacquireAfterDutyChange();
    sync_feedback_update_sequence();
    tach_valid = false;
    last_duty_change_tick = HAL_GetTick();
    state = APP_FAN_STATE_RUNNING;
    return true;
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppFan_Process(void)
{
    if (!init_ok) return;

    if (state == APP_FAN_STATE_STARTUP_BOOST)
    {
        if ((HAL_GetTick() - boost_start_tick) >= APP_FAN_STARTUP_BOOST_MS)
        {
            if (!apply_duty(target_duty))
            {
                apply_duty(0U);
                state   = APP_FAN_STATE_PWM_ERROR;
                enabled = false;
                return;
            }
            applied_duty = target_duty;
            AppFanFeedback_ReacquireAfterDutyChange();
            sync_feedback_update_sequence();
            tach_valid = false;
            state = APP_FAN_STATE_TACH_UNRELIABLE;
            last_duty_change_tick = HAL_GetTick();
        }
        return;
    }

    if (!enabled) return;
    if (applied_duty == 0U) return;

    uint32_t now = HAL_GetTick();
    AppFanFeedbackSnapshot fs;

    if (AppFanFeedback_GetSnapshot(&fs) && (fs.state == 1U))
    {
        if (fs.update_seq != last_fg_update_seq)
        {
            last_fg_fmhz   = fs.freq_millihz;
            last_rpm       = calc_rpm(fs.freq_millihz);
            last_tach_tick = now;
            last_fg_update_seq = fs.update_seq;
            tach_valid = true;
            ever_had_valid_tach = true;

            state = APP_FAN_STATE_RUNNING;
        }
        else
        {
            if (ever_had_valid_tach &&
                (now - last_tach_tick) >= APP_FAN_PERSISTENT_NO_TACH_MS)
            {
                state = APP_FAN_STATE_NO_TACH;
                last_fg_fmhz = 0U;
                last_rpm     = 0U;
                tach_valid   = false;
            }
            else if (!ever_had_valid_tach &&
                     !is_reacquire_grace_active(now))
            {
                state = APP_FAN_STATE_NO_TACH;
                last_fg_fmhz = 0U;
                last_rpm     = 0U;
                tach_valid   = false;
            }
        }
    }
    else
    {
        tach_valid = false;

        if (ever_had_valid_tach &&
            (now - last_tach_tick) >= APP_FAN_PERSISTENT_NO_TACH_MS)
        {
            state = APP_FAN_STATE_NO_TACH;
            last_fg_fmhz = 0U;
            last_rpm     = 0U;
            tach_valid   = false;
        }
        else if (!ever_had_valid_tach &&
                 !is_reacquire_grace_active(now))
        {
            state = APP_FAN_STATE_NO_TACH;
            last_fg_fmhz = 0U;
            last_rpm     = 0U;
            tach_valid   = false;
        }
        else if (applied_duty > 0U && !is_grace_period_active())
        {
            state = APP_FAN_STATE_TACH_UNRELIABLE;
        }
    }
}

/**
 * @brief 更新启用状态和目标参数。
 * @param en 见调用点；该参数只在本次调用期间有效。
 * @param duty 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_SetEnabled(bool en, uint16_t duty)
{
    if (!init_ok) return false;

    if (en && (safety_fault_latched || restart_inhibited))
        return false;

    if (!en)
    {
        if (duty != 0U) return false;
        handle_disable();
        return true;
    }

    if (duty == 0U)
    {
        if (!enabled)
        {
            AppFanFeedback_ResetMeasurement();
            sync_feedback_update_sequence();
        }
        if (!apply_duty(0U)) { state = APP_FAN_STATE_PWM_ERROR; enabled = false; return false; }
        applied_duty     = 0U;
        enabled          = true;
        state            = APP_FAN_STATE_OFF;
        target_duty      = 0U;
        last_fg_fmhz     = 0U;
        last_rpm         = 0U;
        last_tach_tick   = 0U;
        tach_valid       = false;
        ever_had_valid_tach = false;
        return true;
    }

    if ((duty < APP_FAN_MIN_DUTY_X100) || (duty > APP_FAN_MAX_DUTY_X100))
        return false;

    if (!enabled)
        return handle_fresh_enable(duty);

    return handle_duty_update(duty);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_TripSafetyFault(void)
{
    bool shutdown_ok;

    safety_fault_latched = true;
    restart_inhibited = true;

    if (!init_ok)
        return false;

    shutdown_ok = apply_duty(0U);
    applied_duty = 0U;
    target_duty = 0U;
    enabled = false;
    last_fg_fmhz = 0U;
    last_rpm = 0U;
    last_tach_tick = 0U;
    tach_valid = false;
    ever_had_valid_tach = false;
    state = shutdown_ok ? APP_FAN_STATE_SAFETY_LOCKED
                        : APP_FAN_STATE_PWM_ERROR;
    return shutdown_ok;
}

/**
 * @brief 清除锁存或历史状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_ClearSafetyFault(void)
{
    if (!init_ok)
        return false;

    if (!apply_duty(0U))
    {
        state = APP_FAN_STATE_PWM_ERROR;
        enabled = false;
        return false;
    }

    safety_fault_latched = false;
    restart_inhibited = true;
    enabled = false;
    target_duty = 0U;
    applied_duty = 0U;
    last_fg_fmhz = 0U;
    last_rpm = 0U;
    last_tach_tick = 0U;
    tach_valid = false;
    ever_had_valid_tach = false;
    state = APP_FAN_STATE_OFF;
    return true;
}

/**
 * @brief 在显式操作员命令后解除重启禁止。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_AuthorizeRestart(void)
{
    if (!init_ok || safety_fault_latched)
        return false;

    restart_inhibited = false;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_IsSafetyFaultLatched(void)
{
    return safety_fault_latched;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_IsRestartInhibited(void)
{
    return restart_inhibited;
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param s 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppFan_GetSnapshot(AppFanSnapshot *s)
{
    if (s == NULL) return false;

    s->state               = state;
    s->enabled             = enabled;
    s->target_duty_x100    = target_duty;
    s->applied_duty_x100   = applied_duty;
    s->pwm_frequency_hz    = APP_FAN_PWM_FREQUENCY_HZ;
    s->fg_frequency_millihz = last_fg_fmhz;
    s->rpm                 = last_rpm;
    s->tach_valid          = tach_valid ? 1U : 0U;

    uint32_t age = (state == APP_FAN_STATE_OFF || last_tach_tick == 0U)
                   ? 65535U
                   : HAL_GetTick() - last_tach_tick;
    s->tach_age_ms = (age > 65535U) ? 65535U : (uint16_t)age;

    return true;
}
