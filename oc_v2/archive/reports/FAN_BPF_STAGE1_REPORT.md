# 风机ADC带通滤波改造——第一阶段报告

## 1. 本阶段范围

本阶段只把新的数字带通滤波器接入ADC测速数据流，并以“影子模式”并行运行。

正式RPM输出仍完全由原有 `app_fan_feedback_adc.c` 包络/边沿算法产生。新滤波器不会修改：

- `AppFanFeedbackSnapshot.state`
- `freq_millihz`
- `period_samples`
- `update_seq`
- 风机失速/无测速判定
- 自动控制反馈
- W2对象0x06、0x09
- PWM输出

代码中设置了编译期安全锁：

```c
#define APP_FAN_FEEDBACK_BPF_DRIVES_OUTPUT 0U
```

若改成非0，`app_fan_feedback_bpf.c` 将直接编译报错。本阶段未实现零交叉、周期测量或RPM切换。

## 2. ADC数据流审计

当前数据流为：

```text
TIM2 TRGO
  -> ADC1扫描PA0(CH0)和PB1(CH9)
  -> DMA循环缓冲区1024个uint16_t
  -> AppAdcScan_Process()
  -> AppFanFeedback_Process()读取PA0样本
```

### 实际采样率

系统时钟配置：

- SYSCLK/HCLK：168 MHz
- APB1：HCLK/4 = 42 MHz
- APB1定时器时钟：84 MHz（APB分频不为1时定时器时钟乘2）
- TIM2 Prescaler：0
- TIM2 ARR：8399

因此：

```text
84 MHz / (0 + 1) / (8399 + 1) = 10,000 Hz
```

TIM2每次触发ADC完成两通道扫描，PA0在每次触发中采样一次，所以PA0有效采样率为10 kHz。

DMA完整缓冲区包含：

- 1024个半字；
- PA0与PB1交错；
- 每个完整块包含512个PA0样本；
- 每块PA0数据时间约51.2 ms。

新滤波器在主循环的 `AppFanFeedback_Process()` 中逐块运行，不在ADC中断中做浮点运算。

## 3. 原测速行为

原算法保持不变：

```text
PA0原始ADC
 -> 10点移动平均
 -> 窗口包络
 -> 35%/65%动态迟滞阈值
 -> 连续样本确认边沿
 -> 相邻上升沿周期
 -> 正式频率/RPM
```

PWM占空比改变时，现有 `app_fan.c` 会调用 `AppFanFeedback_ResetMeasurement()`。本阶段只在同一个复位入口同步重置影子滤波器，没有改变旧算法的调用关系或状态语义。

## 4. 新影子滤波器

参数：

- PA0采样率：10 kHz
- 类型：Butterworth带通
- 数字总阶数：4阶
- 实现：两个二阶SOS级联
- 运行格式：float32
- 结构：Direct Form II Transposed
- 截止频率：20 Hz、100 Hz
- 预热时间：150 ms（1500个PA0样本）

滤波器没有使用CMSIS-DSP。当前工程虽包含CMSIS核心头文件，但没有集成CMSIS-DSP库；为避免扩大工程修改，采用独立的两节DF2T实现。

工程编译参数已启用F407单精度FPU：

```text
-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
```

## 5. SOS系数

系数由以下脚本生成并验证：

```text
tools/design_fan_bpf.py
```

设计调用：

```python
scipy.signal.butter(
    2,
    [20, 100],
    btype="bandpass",
    fs=10000,
    output="sos",
)
```

注意：SciPy带通原型阶数为2，转换后的数字带通总阶数为4，共两个二阶节。

系数顺序为：

```text
[b0, b1, b2, a0, a1, a2]
```

浮点系数：

```text
SOS1:
0.0006098547019, 0.001219709404, 0.0006098547019,
1.0, -1.941980720, 0.9449790716

SOS2:
1.0, -2.0, 1.0,
1.0, -1.985410213, 0.9856109619
```

运行公式：

```c
y      = b0 * x + s1;
s1_new = b1 * x - a1 * y + s2;
s2_new = b2 * x - a2 * y;
```

该符号约定与SciPy SOS分母 `1 + a1*z^-1 + a2*z^-2` 一致。

## 6. 频响验证

完整报告：

```text
../../reference/filter_design/fan_bpf_frequency_response.md
```

关键点：

| 频率 | 增益/衰减 |
|---:|---:|
| 5 Hz | -27.75 dB |
| 10 Hz | -15.16 dB |
| 20 Hz | -3.01 dB |
| 30 Hz | -0.19 dB |
| 33.33 Hz | -0.05 dB |
| 50 Hz | 约0 dB |
| 76.67 Hz | -0.64 dB |
| 80 Hz | -0.88 dB |
| 100 Hz | -3.01 dB |
| 150 Hz | -9.79 dB |
| 300 Hz | -22.64 dB |
| 1 kHz | -44.42 dB |

实际风机工作范围33.33～76.67 Hz内，float32系数的增益范围约为0～-0.643 dB。

群延迟：

| 频率 | 群延迟 |
|---:|---:|
| 33.33 Hz | 约8.65 ms |
| 50 Hz | 约5.14 ms |
| 76.67 Hz | 约4.55 ms |

最大极点半径：

```text
float64: 0.992779398
float32: 0.992779413
```

均小于1，float32量化后保持稳定。

## 7. 影子诊断

新增内部统计结构 `AppFanFeedbackBpfStats`：

- `enabled`
- `warmed_up`
- `processed_samples`
- `warmup_remaining_samples`
- `output_last`
- `block_min`
- `block_max`
- `block_peak_to_peak`
- `block_rms`
- `block_samples`
- `nonfinite_count`
- `reset_count`

可通过：

```c
AppFanFeedbackBpf_GetStats()
```

读取。没有修改任何W2协议字段。

若输出出现NaN/Inf：

- `nonfinite_count`增加；
- 滤波器状态自动清零；
- 重新进入150 ms预热；
- 正式旧测速算法不受影响。

## 8. 编译开关

默认开启影子运行：

```c
#define APP_FAN_FEEDBACK_BPF_SHADOW_ENABLE 1U
```

关闭方式：

```text
-DAPP_FAN_FEEDBACK_BPF_SHADOW_ENABLE=0
```

集成调用均由预处理条件包围。关闭后，`app_fan_feedback_adc.o` 中不存在对BPF函数的未解析引用，旧测速路径保持原样。

## 9. 性能估算

每个PA0样本经过两个二阶节。每节约：

- 5次浮点乘法；
- 4次浮点加减。

10 kHz采样时约：

- 100,000次浮点乘法/秒；
- 80,000次浮点加减/秒；
- 每51.2 ms计算一次 `sqrtf()` 用于影子RMS。

STM32F407硬件FPU承担该负载，预计占用较低。

静态数据净载荷：

- SOS状态：16字节；
- 统计结构：44字节；
- 平方和：4字节；
- 初始化标志：1字节；
- SOS系数：40字节Flash。

实际RAM对齐和代码尺寸以ARM目标编译器输出为准。

## 10. 离线测试

可执行：

```text
Tests/run_fan_bpf_host_test.sh
```

当前环境测试结果：

```text
DC RMS: 0.024994
Gain 10 Hz: 0.174531
Gain 33.33 Hz: 0.994117
Gain 50 Hz: 0.999713
Gain 76.67 Hz: 0.928682
Gain 150 Hz: 0.323905
50 Hz + 1 kHz composite RMS: 353.439972
33.33 Hz + 1 Hz baseline drift RMS: 563.291321
fan BPF host tests passed
```

覆盖：

- 直流抑制；
- 10/33.33/50/76.67/150 Hz；
- 50 Hz有效信号叠加1 kHz干扰；
- 33.33 Hz有效信号叠加1 Hz基线漂移；
- 150 ms预热状态；
- 非有限值计数保持为0。

还完成了：

- BPF开启时C11 `-Wall -Wextra -Werror` 编译检查；
- BPF关闭时C11 `-Wall -Wextra -Werror` 编译检查；
- BPF关闭时旧反馈对象无BPF函数引用检查。

## 11. 目标工程构建状态

当前执行环境没有 `arm-none-eabi-gcc` 和PowerShell，因此没有重新生成Master/Slave ELF、HEX和BIN，也不能给出修改后的ARM `text/data/bss`。

上传工程中原有构建产物的基线尺寸为：

| 角色 | text | data | bss | total |
|---|---:|---:|---:|---:|
| Master | 55676 | 384 | 6424 | 62484 |
| Slave | 48388 | 384 | 6920 | 55692 |

`build.ps1` 已加入：

```text
Core/Src/app_fan_feedback_bpf.c
```

在Windows工具链环境中需要重新执行：

```powershell
.\build.ps1 -Role master
.\build.ps1 -Role slave
```

目标构建通过后再烧录硬件。

## 12. 硬件影子验证步骤

本阶段烧录后，正式RPM显示应与修改前一致。

建议固定以下工况，每个保持10～30秒：

- 风机关闭；
- 约1000 RPM；
- 约1600 RPM；
- 约2300 RPM；
- 当前最容易出现原测速异常的低占空比。

通过调试器观察 `bpf_stats` 或调用Getter，检查：

1. `enabled == 1`；
2. 约150 ms后 `warmed_up == 1`；
3. `processed_samples`持续增加；
4. `nonfinite_count == 0`；
5. 风机停止时，预热完成后 `block_rms` 和Vpp应明显低于运行时；
6. 低占空比运行时，即使原RPM偶尔无效，带通输出仍应保持可辨识的周期性幅值；
7. 正式RPM、自动控制、保护状态和W2数据与修改前一致。

## 13. 尚未实施

以下内容留到第二阶段：

- 带迟滞的正负过零检测；
- 零交叉插值；
- 周期范围判定；
- 最近5周期中位数；
- 幅值/RMS有效门限；
- 新旧测速结果对比诊断；
- 新算法接管RPM输出；
- 旧算法回退策略。

完成影子硬件观察后，再进入第二阶段。
