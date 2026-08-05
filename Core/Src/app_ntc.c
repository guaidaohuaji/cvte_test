/**
 * @file app_ntc.c
 * @brief NTC 温度换算与状态分类（实现文件）。
 *
 * 模块职责：周期读取共享 ADC 平均值，计算电压、热敏电阻阻值并按厂家 Rcent 表换算温度，并提供控制温度与故障状态。
 * 数据输入：AppAdcScan_GetNtcAverage() 提供的 ADC 平均值和样本数。
 * 数据输出：ADC、电压、阻值、温度、范围状态和有效性快照。
 * 执行上下文：主循环非阻塞周期任务；配置和数学异常通过状态返回，不阻塞其他功能。
 * 阅读重点：先看 app_ntc_config.h 的电阻网络，再顺着 do_sample()、calc_resistance() 和 calc_temp() 阅读查表、插值与范围判定。
 *
 * 约束：本工程采用裸机超级循环。除明确标注的 HAL 短帧发送外，业务
 *       状态机不得主动延时等待；中断回调只做最小工作，复杂处理放到主循环。
 */

#include "app_ntc.h"
#include "app_ntc_config.h"
#include "app_adc_scan.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>

static AppNtcSnapshot snap;
static uint32_t last_sample_tick;
static uint32_t last_ok_tick;
static bool init_ok;

/*
 * LTR LNTD5.06(05)GW 的厂家 R-T 表 Rcent 列。
 *
 * - 索引 0 对应 -40°C，索引 160 对应 120°C；相邻项温差固定 1°C。
 * - 数据表单位为 kΩ，这里离线转换为整数 Ω，避免运行时浮点和 logf()。
 * - NTC 为负温度系数器件，因此阻值严格随温度升高而下降。
 * - 表外阻值不外推：高于首项钳位 -40°C，低于末项钳位 120°C。
 */
#define APP_NTC_TABLE_MIN_TEMP_CENTI_C  (-4000)
#define APP_NTC_TABLE_MAX_TEMP_CENTI_C   12000
#define APP_NTC_TABLE_STEP_CENTI_C         100
#define APP_NTC_TABLE_POINT_COUNT          161U

static const uint32_t ntc_rcent_ohm[APP_NTC_TABLE_POINT_COUNT] =
{
     64069U,  60108U,  56412U,  52962U,  49741U,  46732U,  43920U,  41293U,
     38836U,  36537U,  34387U,  32375U,  30490U,  28725U,  27072U,  25522U,
     24069U,  22707U,  21429U,  20229U,  19103U,  18046U,  17052U,  16119U,
     15242U,  14417U,  13641U,  12911U,  12224U,  11577U,  10968U,  10394U,
      9854U,   9344U,   8864U,   8410U,   7983U,   7579U,   7199U,   6839U,
      6499U,   6178U,   5875U,   5588U,   5317U,   5060U,   4817U,   4587U,
      4370U,   4164U,   3969U,   3784U,   3608U,   3442U,   3284U,   3135U,
      2993U,   2858U,   2730U,   2609U,   2494U,   2384U,   2280U,   2181U,
      2087U,   1997U,   1912U,   1831U,   1754U,   1680U,   1610U,   1543U,
      1480U,   1419U,   1361U,   1306U,   1254U,   1204U,   1156U,   1110U,
      1067U,   1025U,    985U,    947U,    911U,    876U,    843U,    811U,
       781U,    752U,    724U,    697U,    672U,    647U,    624U,    602U,
       580U,    559U,    540U,    521U,    503U,    485U,    468U,    452U,
       437U,    422U,    408U,    394U,    381U,    369U,    357U,    345U,
       334U,    323U,    313U,    303U,    293U,    284U,    275U,    266U,
       258U,    250U,    242U,    235U,    228U,    221U,    214U,    208U,
       202U,    196U,    190U,    185U,    179U,    174U,    169U,    164U,
       160U,    155U,    151U,    147U,    142U,    139U,    135U,    131U,
       128U,    124U,    121U,    118U,    114U,    111U,    108U,    106U,
       103U,    100U,     98U,     95U,     93U,     90U,     88U,     86U,
        84U
};

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool check_config(void)
{
    uint32_t index;

    if ((APP_ADC_FULL_SCALE_COUNTS == 0U) ||
        (APP_NTC_R_TOP_OHM == 0U) ||
        (APP_NTC_R_MID_OHM == 0U) ||
        (APP_NTC_R_BOTTOM_OHM == 0U) ||
        (APP_NTC_SAMPLE_COUNT == 0U) ||
        (APP_ADC_REFERENCE_MV == 0U) ||
        (APP_NTC_MIN_TEMP_CENTI_C != APP_NTC_TABLE_MIN_TEMP_CENTI_C) ||
        (APP_NTC_MAX_TEMP_CENTI_C != APP_NTC_TABLE_MAX_TEMP_CENTI_C))
    {
        return false;
    }

    /* 二分查找依赖严格单调递减；启动时验证一次，防止抄表错误静默生效。 */
    for (index = 1U; index < APP_NTC_TABLE_POINT_COUNT; index++)
    {
        if (ntc_rcent_ohm[index - 1U] <= ntc_rcent_ohm[index])
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 根据当前三电阻分压拓扑和 ADC 码计算 NTC 阻值，显式检查除零和不可能的分母。
 * @param adc 见调用点；该参数只在本次调用期间有效。
 * @param r 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool calc_resistance(uint16_t adc, uint32_t *r)
{
    if (r == NULL || adc == 0U) return false;

    uint64_t top    = (uint64_t)APP_NTC_R_BOTTOM_OHM * (uint64_t)APP_ADC_FULL_SCALE_COUNTS;
    uint64_t total  = (top + (uint64_t)adc / 2ULL) / (uint64_t)adc;
    uint64_t fixed  = (uint64_t)APP_NTC_R_TOP_OHM +
                      (uint64_t)APP_NTC_R_MID_OHM +
                      (uint64_t)APP_NTC_R_BOTTOM_OHM;

    if (total <= fixed) return false;
    total -= fixed;
    if (total > UINT32_MAX) return false;
    *r = (uint32_t)total;
    return true;
}

/**
 * @brief 执行内部数值计算并进行边界检查。
 * @param adc 见调用点；该参数只在本次调用期间有效。
 * @return 返回值含义见函数名、对应枚举或调用点。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static uint16_t calc_voltage_mv(uint16_t adc)
{
    uint32_t v = (uint32_t)adc * APP_ADC_REFERENCE_MV;
    v = (v + APP_ADC_FULL_SCALE_COUNTS / 2U) / APP_ADC_FULL_SCALE_COUNTS;
    return (uint16_t)v;
}

/**
 * @brief 按厂家 Rcent 表把 NTC 阻值换算为 0.01°C，并报告是否发生端点钳位。
 * @param out_temp_centi_c 输出温度，单位 0.01°C。
 * @param out_range_status 输出表内、低温钳位或高温钳位状态。
 * @param r_ohm 根据分压网络计算得到的 NTC 阻值，单位 Ω。
 * @return true 表示查表和插值成功；false 表示参数或表配置异常。
 * @note 表项每 1°C 一个点；表内使用二分查找和 64 位整数线性插值。
 */
static bool calc_temp(
    int16_t *out_temp_centi_c,
    AppNtcRangeStatus *out_range_status,
    uint32_t r_ohm)
{
    uint32_t low;
    uint32_t high;
    uint32_t r_cold;
    uint32_t r_hot;
    uint32_t delta_r;
    uint64_t offset_num;
    int32_t temp_centi_c;

    if ((out_temp_centi_c == NULL) ||
        (out_range_status == NULL) ||
        (r_ohm == 0U))
    {
        return false;
    }

    /* 比 -40°C 更冷时阻值更大；不做曲线外推，保持原有钳位语义。 */
    if (r_ohm > ntc_rcent_ohm[0])
    {
        *out_temp_centi_c = (int16_t)APP_NTC_TABLE_MIN_TEMP_CENTI_C;
        *out_range_status = APP_NTC_RANGE_CLAMPED_LOW;
        return true;
    }

    /* 比 120°C 更热时阻值更小。 */
    if (r_ohm < ntc_rcent_ohm[APP_NTC_TABLE_POINT_COUNT - 1U])
    {
        *out_temp_centi_c = (int16_t)APP_NTC_TABLE_MAX_TEMP_CENTI_C;
        *out_range_status = APP_NTC_RANGE_CLAMPED_HIGH;
        return true;
    }

    if (r_ohm == ntc_rcent_ohm[0])
    {
        *out_temp_centi_c = (int16_t)APP_NTC_TABLE_MIN_TEMP_CENTI_C;
        *out_range_status = APP_NTC_RANGE_IN_RANGE;
        return true;
    }

    if (r_ohm == ntc_rcent_ohm[APP_NTC_TABLE_POINT_COUNT - 1U])
    {
        *out_temp_centi_c = (int16_t)APP_NTC_TABLE_MAX_TEMP_CENTI_C;
        *out_range_status = APP_NTC_RANGE_IN_RANGE;
        return true;
    }

    /*
     * 查找满足 table[low] >= r_ohm >= table[high] 的相邻点。
     * 表严格递减，所以“阻值仍大于等于目标”时向热端移动 low。
     */
    low = 0U;
    high = APP_NTC_TABLE_POINT_COUNT - 1U;
    while ((high - low) > 1U)
    {
        uint32_t middle = low + (high - low) / 2U;

        if (ntc_rcent_ohm[middle] >= r_ohm)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }

    r_cold = ntc_rcent_ohm[low];
    r_hot = ntc_rcent_ohm[high];
    if ((r_cold <= r_hot) ||
        (r_ohm > r_cold) ||
        (r_ohm < r_hot))
    {
        return false;
    }

    delta_r = r_cold - r_hot;
    offset_num = (uint64_t)(r_cold - r_ohm) *
                 (uint64_t)APP_NTC_TABLE_STEP_CENTI_C;

    /* 加半个分母，实现最接近 0.01°C 的整数舍入。 */
    temp_centi_c = APP_NTC_TABLE_MIN_TEMP_CENTI_C +
                   (int32_t)(low * (uint32_t)APP_NTC_TABLE_STEP_CENTI_C) +
                   (int32_t)((offset_num + (uint64_t)delta_r / 2ULL) /
                             (uint64_t)delta_r);

    if ((temp_centi_c < INT16_MIN) || (temp_centi_c > INT16_MAX))
    {
        return false;
    }

    *out_temp_centi_c = (int16_t)temp_centi_c;
    *out_range_status = APP_NTC_RANGE_IN_RANGE;
    return true;
}

/**
 * @brief 内部辅助函数；将复杂逻辑拆分为可测试、可复用的小步骤。
 * @param adc_out 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static bool do_sample(uint16_t *adc_out)
{
    uint32_t ntc_count = 0U;
    if (adc_out == NULL) return false;
    if (!AppAdcScan_GetNtcAverage(adc_out, &ntc_count)) return false;
    return true;
}

/**
 * @brief 更新内部状态或硬件配置。
 * @note 文件内部辅助函数，不属于对外 API。
 */
static void set_sensor_fault(void)
{
    snap.sensor_measurement_valid = false;
    snap.range_status = APP_NTC_RANGE_SENSOR_FAULT;
    snap.control_temp_centi_c = 0;
}

/**
 * @brief 应用查表结果；测量有效范围为 -40~120°C，表外仅钳位显示值。
 * @param tc 查表得到的温度，单位 0.01°C。
 * @param range_status 查表返回的范围状态。
 * @note AUTO 控制范围不在此处定义，仍由 app_auto_control_config.h 独立限制为 -25~60°C。
 */
static void apply_range_status(int16_t tc, AppNtcRangeStatus range_status)
{
    snap.sensor_measurement_valid = true;
    snap.range_status = range_status;
    snap.control_temp_centi_c = tc;
}

/**
 * @brief 初始化模块硬件依赖和运行时状态。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppNtc_Init(void)
{
    snap.state = APP_NTC_STATE_SEARCH;
    snap.adc_raw = 0U;
    snap.voltage_mv = 0U;
    snap.resistance_ohm = 0U;
    snap.temp_centi_c = 0;
    snap.age_ms = 65535U;
    snap.sensor_measurement_valid = false;
    snap.range_status = APP_NTC_RANGE_SENSOR_FAULT;
    snap.control_temp_centi_c = 0;
    last_sample_tick = 0U;
    last_ok_tick = 0U;
    init_ok = false;

    if (!check_config())
    {
        snap.state = APP_NTC_STATE_CONFIG_ERROR;
        set_sensor_fault();
        return false;
    }

    init_ok = true;
    return true;
}

/**
 * @brief 执行一次非阻塞主循环处理。
 */
void AppNtc_Process(void)
{
    if (!init_ok) return;

    uint32_t now = HAL_GetTick();
    if ((now - last_sample_tick) < APP_NTC_SAMPLE_INTERVAL_MS) return;
    last_sample_tick = now;

    uint16_t adc = 0U;
    if (!do_sample(&adc))
    {
        snap.state = APP_NTC_STATE_ADC_ERROR;
        set_sensor_fault();
        return;
    }

    snap.adc_raw    = adc;
    snap.voltage_mv = calc_voltage_mv(adc);

    uint32_t r = 0U;
    if (!calc_resistance(adc, &r))
    {
        snap.resistance_ohm = 0U;
        snap.temp_centi_c   = 0;
        if (adc == 0U)
        {
            snap.state = APP_NTC_STATE_OPEN_OR_UNDER_TEMP;
        }
        else
        {
            snap.state = APP_NTC_STATE_SHORT_OR_OVER_TEMP;
        }
        set_sensor_fault();
        return;
    }

    snap.resistance_ohm = r;

    int16_t tc = 0;
    AppNtcRangeStatus range_status = APP_NTC_RANGE_SENSOR_FAULT;
    if (!calc_temp(&tc, &range_status, r))
    {
        snap.temp_centi_c = 0;
        snap.state = APP_NTC_STATE_CALC_ERROR;
        set_sensor_fault();
        return;
    }

    snap.temp_centi_c = tc;
    apply_range_status(tc, range_status);

    if (snap.range_status == APP_NTC_RANGE_IN_RANGE)
    {
        snap.state = APP_NTC_STATE_OK;
        last_ok_tick = now;
    }
    else if (snap.range_status == APP_NTC_RANGE_CLAMPED_LOW)
    {
        snap.state = APP_NTC_STATE_OPEN_OR_UNDER_TEMP;
    }
    else
    {
        snap.state = APP_NTC_STATE_SHORT_OR_OVER_TEMP;
    }
}

/**
 * @brief 复制当前模块快照供上层查询或协议编码。
 * @param s 见调用点；该参数只在本次调用期间有效。
 * @return true 表示操作成功或数据有效；false 表示参数、状态或硬件条件不满足。
 */
bool AppNtc_GetSnapshot(AppNtcSnapshot *s)
{
    if (s == NULL) return false;

    uint32_t now = HAL_GetTick();
    uint32_t age;

    if (snap.state == APP_NTC_STATE_OK)
    {
        age = (now >= last_ok_tick) ? (now - last_ok_tick) : 0U;
    }
    else
    {
        age = 65535U;
    }
    snap.age_ms = (age > 65535U) ? 65535U : (uint16_t)age;

    *s = snap;
    return true;
}
