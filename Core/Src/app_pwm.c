/**
 * @file app_pwm.c
 * @brief TIM4_CH1 通用 PWM 输出（实现文件）。
 *
 * 模块职责：根据目标频率和整数百分比占空比计算 PSC/ARR/CCR，原子更新 TIM4，并保存目标值、实际值和频率误差。
 * 数据输入：对象 0x01 控制命令传入的 enabled、frequency_hz 和 duty_x100。
 * 数据输出：PB6/TIM4_CH1 波形；对象 0x01 查询使用的配置快照。
 * 执行上下文：仅由主循环调用；更新期间会短暂停止 TIM4，并在启动失败时回滚旧配置。
 * 阅读重点：重点理解 period_counts=ARR+1、PSC+1、1%量化、0/100%强制电平以及失败回滚。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_pwm.h"
#include "tim.h"
#include <stddef.h>

#define TIM4_CLOCK_HZ             84000000ULL
#define TIM4_MAX_PERIOD_COUNTS    65536ULL
#define PWM_MIN_PERIOD_COUNTS     100ULL
#define PWM_MAX_DUTY_X100         10000U

typedef enum
{
    PWM_OUTPUT_FORCED_LOW = 0,
    PWM_OUTPUT_PWM,
    PWM_OUTPUT_FORCED_HIGH
} PwmOutputMode;

static uint32_t target_freq = 1000U;
static uint32_t actual_freq = 1000U;
static int32_t freq_ppm = 0;
static uint16_t target_duty_x100 = 5000U;
static uint16_t actual_duty_x100 = 5000U;
static uint32_t current_psc = 83U;
static uint32_t current_arr = 999U;
static uint32_t current_ccr = 500U;
static PwmOutputMode current_mode = PWM_OUTPUT_PWM;
static bool pwm_enabled = false;
static bool pwm_running = false;

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param duty_x100 占空比，单位 0.01%，例如 100 表示 1%。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t quantize_duty_x100(uint16_t duty_x100)
{
    uint32_t quantized;

    if (duty_x100 >= PWM_MAX_DUTY_X100)
    {
        return PWM_MAX_DUTY_X100;
    }

    quantized = ((uint32_t)duty_x100 + (APP_PWM_DUTY_STEP_X100 / 2U)) /
                APP_PWM_DUTY_STEP_X100;
    quantized *= APP_PWM_DUTY_STEP_X100;
    if (quantized > PWM_MAX_DUTY_X100)
    {
        quantized = PWM_MAX_DUTY_X100;
    }
    return (uint16_t)quantized;
}

/**
 * @brief 在 16 位 PSC/ARR 限制内选择计数分频，使实际频率尽量接近请求值，并保证每周期至少 100 个计数以维持 1%占空比分辨率。
 * @param hz 见调用点；该参数只在本次调用期间有效。
 * @param psc_out 见调用点；该参数只在本次调用期间有效。
 * @param arr_out 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool compute_frequency_params(uint32_t hz,
                                     uint32_t *psc_out,
                                     uint32_t *arr_out)
{
    uint64_t psc_div;
    uint64_t period_counts;
    uint64_t denominator;

    if ((psc_out == NULL) || (arr_out == NULL))
    {
        return false;
    }
    if ((hz < APP_PWM_MIN_FREQUENCY_HZ) ||
        (hz > APP_PWM_MAX_FREQUENCY_HZ))
    {
        return false;
    }

    /* 先求能让 ARR 落入 16 位范围的最小预分频除数。向上取整可避免
     * period_counts 超过 65536。 */
    denominator = (uint64_t)hz * TIM4_MAX_PERIOD_COUNTS;
    psc_div = (TIM4_CLOCK_HZ + denominator - 1ULL) / denominator;
    if (psc_div < 1ULL)
    {
        psc_div = 1ULL;
    }
    if (psc_div > 65536ULL)
    {
        return false;
    }

    /* 在选定 PSC 后，对周期计数做四舍五入。ARR 寄存器保存的是
     * period_counts-1，而不是周期计数本身。 */
    denominator = (uint64_t)hz * psc_div;
    period_counts = (TIM4_CLOCK_HZ + (denominator / 2ULL)) / denominator;

    if ((period_counts < PWM_MIN_PERIOD_COUNTS) ||
        (period_counts > TIM4_MAX_PERIOD_COUNTS))
    {
        return false;
    }

    *psc_out = (uint32_t)(psc_div - 1ULL);
    *arr_out = (uint32_t)(period_counts - 1ULL);
    return true;
}

/**
 * @brief 根据输入计算硬件或协议参数。
 * @param arr 见调用点；该参数只在本次调用期间有效。
 * @param duty_x100 占空比，单位 0.01%，例如 100 表示 1%。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint32_t compute_ccr_percent(uint32_t arr, uint16_t duty_x100)
{
    uint64_t period_counts = (uint64_t)arr + 1ULL;
    uint32_t duty_percent = (uint32_t)duty_x100 / APP_PWM_DUTY_STEP_X100;
    uint64_t ccr = (period_counts * duty_percent + 50ULL) / 100ULL;

    if (ccr >= period_counts)
    {
        ccr = period_counts - 1ULL;
    }
    return (uint32_t)ccr;
}

/**
 * @brief 在 PWM1、强制低、强制高三种输出比较模式之间切换，专门处理 0%和100%的真正静态电平。
 * @param mode 见调用点；该参数只在本次调用期间有效。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void set_channel_mode(PwmOutputMode mode)
{
    uint32_t oc_mode;

    switch (mode)
    {
    case PWM_OUTPUT_FORCED_LOW:
        oc_mode = TIM_OCMODE_FORCED_INACTIVE;
        break;
    case PWM_OUTPUT_FORCED_HIGH:
        oc_mode = TIM_OCMODE_FORCED_ACTIVE;
        break;
    case PWM_OUTPUT_PWM:
    default:
        oc_mode = TIM_OCMODE_PWM1;
        break;
    }

    MODIFY_REG(htim4.Instance->CCMR1, TIM_CCMR1_OC1M, oc_mode);
}

/**
 * @brief 完成一次启用状态的原子重配置：先计算全部新参数，再停表、写寄存器、产生更新事件并重启；若重启失败则恢复旧配置。
 * @param freq_hz 频率，单位 Hz。
 * @param requested_duty_x100 请求占空比，单位 0.01%。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool apply_enabled_configuration(uint32_t freq_hz,
                                        uint16_t requested_duty_x100)
{
    uint32_t new_psc;
    uint32_t new_arr;
    uint32_t new_ccr;
    uint16_t quantized_duty;
    uint16_t new_actual_duty;
    uint32_t new_actual_freq;
    int32_t new_ppm;
    PwmOutputMode new_mode;
    uint64_t total_divider;
    int64_t frequency_numerator;

    uint32_t old_psc = current_psc;
    uint32_t old_arr = current_arr;
    uint32_t old_ccr = current_ccr;
    PwmOutputMode old_mode = current_mode;
    bool old_running = pwm_running;

    if (requested_duty_x100 > PWM_MAX_DUTY_X100)
    {
        return false;
    }
    if (!compute_frequency_params(freq_hz, &new_psc, &new_arr))
    {
        return false;
    }

    quantized_duty = quantize_duty_x100(requested_duty_x100);
    if (quantized_duty == 0U)
    {
        new_mode = PWM_OUTPUT_FORCED_LOW;
        new_ccr = 0U;
        new_actual_duty = 0U;
    }
    else if (quantized_duty >= PWM_MAX_DUTY_X100)
    {
        new_mode = PWM_OUTPUT_FORCED_HIGH;
        new_ccr = new_arr;
        new_actual_duty = PWM_MAX_DUTY_X100;
    }
    else
    {
        uint64_t period_counts = (uint64_t)new_arr + 1ULL;
        new_mode = PWM_OUTPUT_PWM;
        new_ccr = compute_ccr_percent(new_arr, quantized_duty);
        new_actual_duty = (uint16_t)
            ((((uint64_t)new_ccr * PWM_MAX_DUTY_X100) +
              (period_counts / 2ULL)) /
             period_counts);
    }

    total_divider = (uint64_t)(new_psc + 1U) * (uint64_t)(new_arr + 1U);
    new_actual_freq = (uint32_t)
        ((TIM4_CLOCK_HZ + (total_divider / 2ULL)) / total_divider);

    frequency_numerator = (int64_t)TIM4_CLOCK_HZ -
                          ((int64_t)freq_hz * (int64_t)total_divider);
    new_ppm = (int32_t)
        ((frequency_numerator * 1000000LL) /
         ((int64_t)freq_hz * (int64_t)total_divider));

    /* 从这里开始才触碰硬件。前面的所有计算均在局部变量中完成，
     * 因而参数非法时不会破坏正在输出的旧波形。 */
    if (pwm_running)
    {
        (void)HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1);
        pwm_running = false;
    }

    __HAL_TIM_SET_PRESCALER(&htim4, new_psc);
    __HAL_TIM_SET_AUTORELOAD(&htim4, new_arr);
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, new_ccr);
    set_channel_mode(new_mode);
    __HAL_TIM_SET_COUNTER(&htim4, 0U);
    htim4.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);

    if (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_SET_PRESCALER(&htim4, old_psc);
        __HAL_TIM_SET_AUTORELOAD(&htim4, old_arr);
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, old_ccr);
        set_channel_mode(old_mode);
        __HAL_TIM_SET_COUNTER(&htim4, 0U);
        htim4.Instance->EGR = TIM_EGR_UG;
        __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);
        if (old_running)
        {
            pwm_running = (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) == HAL_OK);
        }
        return false;
    }

    target_freq = freq_hz;
    actual_freq = new_actual_freq;
    freq_ppm = new_ppm;
    target_duty_x100 = quantized_duty;
    actual_duty_x100 = new_actual_duty;
    current_psc = new_psc;
    current_arr = new_arr;
    current_ccr = new_ccr;
    current_mode = new_mode;
    pwm_enabled = true;
    pwm_running = true;
    return true;
}

/**
 * @brief 把已计算的目标值应用到硬件或下层模块。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool apply_disabled_state(void)
{
    uint32_t old_ccr = current_ccr;
    PwmOutputMode old_mode = current_mode;
    bool old_running = pwm_running;

    if (pwm_running)
    {
        (void)HAL_TIM_PWM_Stop(&htim4, TIM_CHANNEL_1);
        pwm_running = false;
    }

    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, 0U);
    set_channel_mode(PWM_OUTPUT_FORCED_LOW);
    __HAL_TIM_SET_COUNTER(&htim4, 0U);
    htim4.Instance->EGR = TIM_EGR_UG;
    __HAL_TIM_CLEAR_FLAG(&htim4, TIM_FLAG_UPDATE);

    if (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) != HAL_OK)
    {
        __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, old_ccr);
        set_channel_mode(old_mode);
        if (old_running)
        {
            pwm_running = (HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1) == HAL_OK);
        }
        return false;
    }

    current_ccr = 0U;
    current_mode = PWM_OUTPUT_FORCED_LOW;
    actual_duty_x100 = 0U;
    pwm_enabled = false;
    pwm_running = true;
    return true;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwm_Init(void)
{
    return AppPwm_Configure(true, 1000U, 5000U);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param enabled true 表示启用，false 表示关闭。
 * @param frequency_hz 请求频率，单位 Hz。
 * @param duty_x100 占空比，单位 0.01%，例如 100 表示 1%。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwm_Configure(bool enabled, uint32_t frequency_hz, uint16_t duty_x100)
{
    if (!enabled)
    {
        if ((frequency_hz != 0U) || (duty_x100 != 0U))
        {
            return false;
        }
        return apply_disabled_state();
    }

    return apply_enabled_configuration(frequency_hz, duty_x100);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param frequency_hz 请求频率，单位 Hz。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwm_SetFrequency(uint32_t frequency_hz)
{
    return AppPwm_Configure(true, frequency_hz, target_duty_x100);
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param duty_x100 占空比，单位 0.01%，例如 100 表示 1%。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwm_SetDutyX100(uint16_t duty_x100)
{
    return AppPwm_Configure(true, target_freq, duty_x100);
}

/**
 * @brief 启用或停用模块输出。
 * @param enabled true 表示启用，false 表示关闭。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppPwm_Enable(bool enabled)
{
    if (enabled)
    {
        return AppPwm_Configure(true, target_freq, target_duty_x100);
    }
    return AppPwm_Configure(false, 0U, 0U);
}

/** @brief 返回通用 PWM 当前逻辑启用状态。 */
bool AppPwm_IsEnabled(void) { return pwm_enabled; }

/** @brief 返回最近一次成功接受的目标频率。 */
uint32_t AppPwm_GetTargetFrequency(void) { return target_freq; }
/** @brief 返回 PSC/ARR 量化后实际可实现的频率。 */
uint32_t AppPwm_GetActualFrequency(void) { return actual_freq; }
/** @brief 返回实际频率相对目标频率的误差，单位 ppm。 */
int32_t AppPwm_GetFrequencyErrorPpm(void) { return freq_ppm; }

/** @brief 返回按 1%步进量化后的目标占空比，单位 0.01%。 */
uint16_t AppPwm_GetTargetDutyX100(void) { return target_duty_x100; }
/** @brief 返回 CCR/周期计数量化后实际占空比，单位 0.01%。 */
uint16_t AppPwm_GetActualDutyX100(void) { return actual_duty_x100; }

/** @brief 返回当前 TIM4 PSC 寄存器值；实际除数为 PSC+1。 */
uint32_t AppPwm_GetPrescaler(void) { return current_psc; }
/** @brief 返回当前 TIM4 ARR 寄存器值；周期计数为 ARR+1。 */
uint32_t AppPwm_GetAutoReload(void) { return current_arr; }
/** @brief 返回当前 TIM4 CCR1 比较值。 */
uint32_t AppPwm_GetCompare(void) { return current_ccr; }
