# 风机ADC带通滤波测速改造——第二阶段报告

## 1. 本阶段范围

本阶段在第一阶段 `20～100 Hz` 四阶Butterworth带通滤波器之后，增加了一套**影子测速器**：

```text
PA0 ADC原始样本
   ├─→ 原包络/阈值/边沿算法 ─→ 正式FG频率与RPM（保持不变）
   └─→ 20～100 Hz带通
          → 自适应迟滞武装
          → 插值零交叉
          → 周期范围检查
          → 5周期中位数
          → 影子FG频率与RPM（只供诊断）
```

新算法仍不参与：

- `AppFanFeedbackSnapshot` 正式频率和有效标志；
- `AppFanSnapshot.rpm`；
- 风机无测速、失速和故障保护；
- 自动风机控制；
- W2对象 `0x06`、`0x09`；
- PWM输出。

编译期安全锁：

```c
#define APP_FAN_FEEDBACK_BPF_DRIVES_OUTPUT       0U
#define APP_FAN_FEEDBACK_BPF_TACH_DRIVES_OUTPUT  0U
```

任一安全锁改为非零，源码会直接编译报错。

## 2. 目标频率范围

已确认换算关系：

```text
RPM = FG频率 × 30
```

实际转速约 `1000～2300 RPM`，对应：

```text
33.33～76.67 Hz
```

影子测速有效范围留有余量：

```text
30～82 Hz
```

10 kHz采样下，对应周期约：

```text
122～333个采样点
```

带通滤波器仍为 `20～100 Hz`，使实际工作频段不落在截止边缘。

## 3. 零交叉策略

没有直接对 `output >= 0` 做简单过零计数，而是采用：

1. 带通输出先到达 `-H`，武装下一次上升过零；
2. 随后检测从负到正的零交叉；
3. 用相邻两个采样点线性插值，得到Q16.16精度的过零位置；
4. 每个周期最多生成一个上升过零事件。

这样迟滞只负责防止零点附近反复触发，真正的时间戳仍取零点，能够减小幅值变化导致的相位偏差。

插值关系：

```text
fraction = -previous / (current - previous)
zero_position = previous_sample_index + fraction
```

只有发生过零时才执行一次浮点除法，不会在每个样本上执行除法。

## 4. 自适应迟滞

迟滞门限来自带通输出RMS估计：

```text
H = max(2 ADC counts, RMS估计 × 20%)
```

RMS估计每个PA0 DMA块更新一次：

- 幅值下降：本块值占50%，使门限较快下降；
- 幅值上升：本块值占25%，避免瞬时尖峰快速拉高门限。

当前信号存在门限：

```text
block_rms >= 6 ADC counts
```

连续3个低RMS块后才关闭过零检测，约对应 `3 × 51.2 ms`。这些参数目前属于影子诊断参数，不影响现有正式测速。

需要注意：固定带通无法排除带内50 Hz工频干扰。风机停止时若PA0存在较强50 Hz，影子算法仍可能锁定约1500 RPM，因此切换正式输出前必须完成硬件对比测试。

## 5. 周期处理

### 5.1 周期范围

- 小于82 Hz对应最小周期：记为短周期并拒绝；
- 大于30 Hz对应最大周期：记为长周期、清空当前历史并重新同步。

### 5.2 防止高频整数分频假锁定

持续100 Hz信号的周期约100点。若短周期只被忽略但不移动参考过零点，第二个100 Hz周期可能累加成200点，并被误判为50 Hz。

本阶段增加了专门处理：

1. 短周期出现时，参考过零点移动到当前真实观测点；
2. 标记一次“短周期恢复”；
3. 随后的第一个合法区间只用于恢复同步，不进入周期历史；
4. 再下一个正常周期才能被接受。

离线测试已确认100 Hz不会被错误锁定成50 Hz。

### 5.3 中位数与一致性

- 保存最近5个有效周期；
- 至少3个周期后发布影子结果；
- 结果使用周期中位数；
- 新周期与历史中位数偏差超过25%时拒绝；
- 连续2次偏差过大后，认为风速发生真实阶跃，清空旧历史并从新周期重新捕获。

该机制能拒绝单次漏边沿、假边沿，同时允许PWM变化后重新锁定新转速。

### 5.4 超时

连续500 ms没有接受到有效周期时：

- `tach_valid` 清零；
- 影子频率、RPM和周期清零；
- 周期历史清空；
- 下次信号需要重新积累至少3个周期。

正式旧测速超时逻辑未改变。

## 6. PWM变化时的复位语义

原工程在运行中每次占空比变化时，都会完整调用：

```c
AppFanFeedback_ResetMeasurement();
```

这会让影子带通每次重新预热150 ms，不利于观察自动控制的小步调节。

本阶段新增：

```c
AppFanFeedback_ReacquireAfterDutyChange();
```

运行中占空比变化改用该入口。它仍然对**正式旧测速算法执行与之前完全相同的重置**，但保留影子BPF和影子周期历史。

完整重置仍用于：

- 风机首次启动；
- 显式完整测速复位；
- 原有关闭/零占空比初始化路径；
- ADC或滤波器异常路径。

因此正式风机功能和故障保护语义没有改变，变化只作用于尚未接管输出的影子算法。

## 7. 新增影子诊断

`AppFanFeedbackBpfStats` 追加：

- `tach_shadow_enabled`
- `tach_signal_present`
- `tach_armed`
- `tach_valid`
- `tach_freq_millihz`
- `tach_rpm`
- `tach_period_samples`
- `tach_update_seq`
- `tach_hysteresis`
- `tach_rms_estimate`
- `tach_rising_crossings`
- `tach_accepted_periods`
- `tach_short_rejected`
- `tach_post_short_rejected`
- `tach_long_rejected`
- `tach_inconsistent_rejected`
- `tach_resync_count`
- `tach_timeout_count`
- `tach_period_history_count`
- `tach_last_crossing_age_samples`

读取方式仍为：

```c
AppFanFeedbackBpf_GetStats(&stats);
```

没有修改W2协议。硬件测试阶段可直接通过调试器观察静态变量 `bpf_stats`，或在代码中调用Getter。

## 8. 修改文件

```text
Core/Inc/app_fan_feedback_bpf.h
Core/Src/app_fan_feedback_bpf.c
Core/Inc/app_fan_feedback_adc.h
Core/Src/app_fan_feedback_adc.c
Core/Src/app_fan.c
Tests/test_app_fan_feedback_bpf.c
Tests/test_app_fan_feedback_reset.c
Tests/run_fan_bpf_host_test.sh
../../reference/release/NO_FIRMWARE_BINARIES.txt
FAN_BPF_STAGE2_REPORT.md
../test_logs/FAN_BPF_STAGE2_HOST_TEST_LOG.txt
```

## 9. 主机离线测试

执行：

```sh
sh Tests/run_fan_bpf_host_test.sh
```

关键结果：

```text
33.33 Hz  → 33334 mHz，1000 RPM，周期300点
76.67 Hz  → 76666 mHz，2300 RPM，周期130点
50 Hz + 1 Hz基线漂移 + 1 kHz干扰 → 50000 mHz，1500 RPM
固定33.33 Hz、幅值900→120→700 → 最终仍为33333 mHz
33.33 Hz阶跃到60 Hz → 重新同步后60000 mHz
20 Hz → 长周期拒绝，结果无效
100 Hz → 短周期拒绝，不会假锁定50 Hz
50 Hz消失后 → 500 ms超时，结果无效
```

同时通过：

- 第一阶段滤波频响测试；
- 带通+影子测速AddressSanitizer/UndefinedBehaviorSanitizer测试；
- 正式旧测速完整复位会重置BPF；
- PWM变化重捕获不会重置BPF；
- 影子测速关闭配置编译；
- 整个BPF关闭配置编译；
- `app_fan_feedback_adc.c` 主机严格警告编译；
- `app_fan.c` 使用HAL桩的主机严格警告编译。

## 10. 资源估算

使用主机GCC `-O2` 对单独BPF模块做相对比较：

| 版本 | text | bss |
|---|---:|---:|
| 第一阶段BPF | 1211 B | 160 B |
| 第二阶段BPF+影子测速 | 3527 B | 224 B |
| 相对增加 | 2316 B | 64 B |

这是主机目标对象大小，只用于相对估算，不能替代ARM最终链接结果。

运行负载主要增加：

- 每样本少量比较和状态更新；
- 每个有效过零一次浮点除法；
- 每个有效周期最多一次5元素排序；
- 每51.2 ms一次RMS `sqrtf()`，与第一阶段一致。

## 11. 未完成项目

当前环境没有 `arm-none-eabi-gcc`，因此未完成：

- Master ARM编译和链接；
- Slave ARM编译和链接；
- text/data/bss真实目标值；
- HEX/BIN生成；
- 实机波形和转速对比。

发布包不包含历史固件二进制，避免误烧录旧代码。

## 12. 建议硬件验证

本阶段烧录后，正式RPM应与修改前保持一致。通过调试器观察影子统计：

1. 固定约1000 RPM运行30秒；
2. 固定约1600 RPM运行30秒；
3. 固定约2300 RPM运行30秒；
4. 在最容易失败的低PWM占空比运行至少2分钟；
5. 手动改变PWM，观察影子算法是否连续跟踪而不重新预热；
6. 风机停止，确认是否存在50 Hz假锁定；
7. 对比正式RPM和 `tach_rpm`。

重点记录：

```text
block_rms
tach_rms_estimate
tach_hysteresis
tach_signal_present
tach_valid
tach_freq_millihz
tach_rpm
tach_short_rejected
tach_long_rejected
tach_inconsistent_rejected
tach_resync_count
tach_timeout_count
```

建议验收条件：

- 1000～2300 RPM内影子结果持续有效；
- 稳态影子RPM无二倍频或半频跳变；
- 低占空比下影子有效率明显优于旧算法；
- 风机停止时不锁定50 Hz；
- 正式旧算法、自动控制和保护行为没有变化。

完成硬件对比后，下一阶段才考虑把影子结果加入W2诊断，或通过单一编译开关进行受控A/B切换；本阶段没有执行正式输出切换。
