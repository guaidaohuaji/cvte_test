# 风机ADC带通滤波测速改造——第三阶段报告

## 1. 本阶段目标

第三阶段将已经完成离线验证的带通测速器接入正式风机反馈路径，同时保留旧测速器作为并行诊断和可选回退：

```text
PA0 ADC 10 kHz
   ├─→ 20～100 Hz四阶Butterworth带通
   │      → 自适应迟滞过零
   │      → 周期检查
   │      → 5周期中位数
   │      → 正式反馈主源
   │
   └─→ 原移动平均/包络/阈值测速
          → 并行运行
          → BPF无效时可回退
```

本阶段没有删除旧算法，也没有改变W2对象格式、自动控制接口或风机状态机接口。

## 2. 默认输出策略

`Core/Inc/app_fan_feedback_adc.h`中新增：

```c
#define APP_FAN_FEEDBACK_OUTPUT_MODE_LEGACY       0U
#define APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY  1U

#define APP_FAN_FEEDBACK_OUTPUT_MODE \
    APP_FAN_FEEDBACK_OUTPUT_MODE_BPF_PRIMARY

#define APP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE  1U
```

默认行为：

1. BPF测速有效时，正式FG频率来自BPF测速；
2. BPF测速无效、旧测速仍有效时，临时使用旧测速；
3. 两者都无效时，正式测速无效；
4. BPF恢复有效后自动重新取得优先权。

一行回滚方式：

```c
#define APP_FAN_FEEDBACK_OUTPUT_MODE \
    APP_FAN_FEEDBACK_OUTPUT_MODE_LEGACY
```

回滚不需要删除带通代码，也不改变协议。

## 3. 正式输出接入位置

正式输出没有直接侵入`app_fan.c`。源选择在：

```text
Core/Src/app_fan_feedback_adc.c
```

完成，然后仍通过现有接口：

```c
AppFanFeedback_GetSnapshot()
```

返回给`AppFan_Process()`。

因此以下既有路径保持原接口：

```text
AppFanFeedback_GetSnapshot
→ AppFan_Process
→ AppFanSnapshot
→ W2对象0x06/0x09
→ 自动控制和故障判定
```

## 4. 更新序列处理

BPF测速和旧测速各自拥有不同的`update_seq`。本阶段增加独立的正式输出序列：

```text
official_update_seq
```

只有以下情况才递增：

- 当前正式源产生了新的测速结果；
- 正式源从BPF切换到旧测速；
- 正式源从旧测速切回BPF。

重复读取同一个BPF结果不会生成虚假的“新测速”。这保证`AppFan_Process()`原有的新鲜度判断继续有效。

## 5. PWM变化语义

运行中小幅改变PWM时：

- 旧测速器仍按原逻辑重新捕获；
- BPF滤波器、周期历史和正式源序列保持；
- 调占空比后，只有BPF产生新周期时，正式`update_seq`才更新；
- 不会把调占空比前的旧RPM伪装成一条新结果。

完整启动、显式完整复位或ADC异常时：

- BPF状态清零；
- 正式反馈立即无效；
- 重新完成150 ms预热和至少3个有效周期后再发布。

## 6. 新增诊断字段

`AppFanFeedbackSnapshot`内部诊断区新增：

```text
active_source
bpf_tach_valid
legacy_tach_valid
legacy_fallback_active
bpf_freq_millihz
legacy_freq_millihz
source_switch_count
```

`active_source`含义：

```text
0 = 当前无有效测速源
1 = BPF带通测速
2 = 旧包络测速
```

这些字段只在固件内部和调试器中使用，没有加入W2对象，协议字节数没有变化。

## 7. 修改文件

```text
Core/Inc/app_fan_feedback_adc.h
Core/Inc/app_fan_feedback_bpf.h
Core/Src/app_fan_feedback_adc.c
Core/Src/app_fan_feedback_bpf.c
Tests/test_app_fan_feedback_source.c
Tests/test_app_fan_feedback_integration.c
Tests/run_fan_bpf_host_test.sh
NO_FIRMWARE_BINARIES.txt
FAN_BPF_STAGE3_REPORT.md
FAN_BPF_STAGE3_HOST_TEST_LOG.txt
FAN_BPF_STAGE3_SANITIZER_LOG.txt
```

`app_fan.c`、自动控制、UART/W2、单总线、风门和温度模块未修改。

## 8. 主机测试结果

执行：

```sh
sh Tests/run_fan_bpf_host_test.sh
```

通过项目：

- 原BPF频响测试；
- 33.33 Hz与76.67 Hz边界测速；
- 基线漂移和1 kHz干扰测试；
- 幅值900→120→700变化测试；
- 33.33→60 Hz阶跃重同步；
- 20 Hz、100 Hz越界拒绝；
- 信号消失500 ms失效；
- 完整复位和PWM重捕获语义；
- BPF主源选择；
- 相同BPF序列不重复发布；
- BPF新结果更新正式序列；
- BPF失效时旧算法回退；
- BPF恢复后重新取得优先权；
- 一行旧算法回滚模式；
- 禁用旧算法回退模式；
- ADC→BPF→正式快照全链路50 Hz测试；
- 信号幅值700降至90后仍稳定测得50 Hz；
- 信号消失后正式输出最终失效。

主机测试结论：

```text
fan BPF stage-3 host verification passed
```

AddressSanitizer和UndefinedBehaviorSanitizer也通过。

## 9. 资源变化估算

使用主机GCC `-Os`对相关目标文件做相对比较：

| 模块 | step13 text | step14 text | 变化 | step13 bss | step14 bss | 变化 |
|---|---:|---:|---:|---:|---:|---:|
| app_fan_feedback_adc | 2484 | 2975 | +491 B | 324 | 532 | +208 B |
| app_fan_feedback_bpf | 2479 | 2479 | 0 B | 224 | 224 | 0 B |

这是主机编译器的相对估算，不等同于ARM最终尺寸。新增BSS主要来自正式快照副本和源仲裁状态。

## 10. 未完成的构建验证

当前环境没有：

```text
arm-none-eabi-gcc
PowerShell
```

因此未生成Master/Slave HEX和BIN，也没有宣称ARM工程已构建通过。

请在Windows工程根目录执行：

```powershell
.\build.ps1 -Role master
.\build.ps1 -Role slave
```

确认：

```text
0 error
无新增warning
```

再烧录新固件。

## 11. 实机验证重点

### 11.1 基本转速

固定占空比分别测试：

```text
约1000 RPM
约1600 RPM
约2300 RPM
低占空比故障工况
```

确认：

```text
active_source = 1
bpf_tach_valid = 1
正式RPM稳定
```

低占空比时允许旧测速无效，但BPF测速和正式RPM应持续有效。

### 11.2 回退验证

可临时制造BPF信号不足或观察启动过程，确认：

```text
BPF无效 + 旧测速有效
→ active_source = 2
→ legacy_fallback_active = 1
```

BPF恢复后应自动回到：

```text
active_source = 1
```

### 11.3 风机停止和50 Hz干扰

固定带通包含50 Hz，这是当前最重要的硬件风险。

风机停止或FG断开时必须确认：

```text
正式tach_valid最终变为0
不会长期显示约1500 RPM
```

若停止时错误锁定约1500 RPM，先使用一行回滚宏恢复旧测速正式输出，再进行50 Hz抑制设计，不要继续依赖当前BPF主源。

### 11.4 连续测试

建议每个工况至少运行5分钟，并记录：

```text
active_source
bpf_tach_valid
legacy_tach_valid
legacy_fallback_active
bpf_freq_millihz
legacy_freq_millihz
source_switch_count
tach_short_rejected
tach_long_rejected
tach_inconsistent_rejected
tach_timeout_count
```

验收目标：

```text
正常运行时active_source长期为1
低占空比正式RPM无掉零和大幅跳变
风机停止后不产生50 Hz假测速
自动控制不触发异常100%保护
```

## 12. 结论

第三阶段已经完成从影子测速到正式反馈的受控切换：

```text
BPF测速作为主源
+ 旧测速并行保留
+ 旧测速可自动回退
+ 一行宏可完整回滚
```

源码级和主机离线测试已通过；ARM构建与真实风机硬件验证仍需在目标环境完成。
