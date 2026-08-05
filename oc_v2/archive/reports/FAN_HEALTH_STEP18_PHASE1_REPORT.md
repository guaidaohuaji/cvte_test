# Step18 Phase 1：风机健康诊断影子模式报告

## 1. 本阶段目标

基于step17增加独立的风机健康诊断，但暂时不执行停机、不锁定控制命令、不修改W2协议和上位机。

本阶段用于验证以下判断逻辑在真实风机上是否会误报：

```text
实际输出PWM
→ 标定表反向插值得到预期RPM
→ 与有效测速RPM比较
→ 偏差或测速丢失持续5秒
→ 锁存内部影子故障候选
```

编译保护：

```c
#define APP_FAN_HEALTH_DRIVES_OUTPUT 0U
```

任何非零配置都会触发编译错误，因此本阶段不会关闭风机。

## 2. 源码审计结论

### 2.1 必须使用实际PWM

诊断使用`AppFanSnapshot.applied_duty_x100`，不使用目标PWM或目标RPM。原因是启动加力、AUTO调节和手动定速期间，目标值可能与物理输出不同。

### 2.2 现有无测速保护会切到100%

AUTO和手动定速控制在底层进入`APP_FAN_STATE_NO_TACH`后，会把目标PWM切到100%。调用`AppFan_SetEnabled()`后，底层状态又会变成`RUNNING`，但`tach_valid`仍为0。

因此新诊断不能简单地在PWM超过80%后停止判断。实现会保存最后一个10%～80%的正常参考占空比，在出现“100%且测速无效”时继续累计测速丢失时间。

### 2.3 主循环接入位置

```text
AppFan_Process
→ ADC/反馈处理
→ AUTO控制
→ 手动风机控制
→ AppFanHealth_Process
```

健康诊断位于所有控制器之后，可以看到本轮最终实际输出；大幅PWM变化会自动进入稳定等待，避免使用旧转速误判新PWM。

## 3. 新增文件

```text
Core/Inc/app_fan_health.h
Core/Inc/app_fan_health_config.h
Core/Src/app_fan_health.c
Tests/test_app_fan_health.c
Tests/run_fan_health_phase1_host_test.sh
```

同时修改：

```text
Core/Inc/app_auto_fan_profile.h
Core/Src/main.c
build.ps1
oc_v2/*
```

## 4. PWM到预期RPM反向插值

复用step15标定表：

| PWM | 预期RPM |
|---:|---:|
| 10% | 1000 |
| 15% | 1200 |
| 20% | 1400 |
| 30% | 1700 |
| 40% | 1900 |
| 50% | 2100 |
| 60% | 2200 |
| 80% | 2300 |

相邻点之间线性插值。例如：

```text
35% → 1800 RPM
45% → 2000 RPM
55% → 2150 RPM
70% → 2250 RPM
```

新增接口：

```c
AppAutoFan_EstimateRpmFromDutyX100()
```

## 5. 判断参数

```text
偏差进入门限：        600 RPM
偏差恢复门限：        450 RPM
故障候选确认时间：    5000 ms
启动加力期间：        不检测
启动加力结束等待：    5000 ms
大幅PWM变化门限：     5.00%
大幅PWM变化等待：     3000 ms
有效诊断PWM范围：     10.00%～80.00%
```

### 5.1 转速偏差

```text
有符号偏差 = 实际RPM - 预期RPM
```

- 偏差绝对值大于600 RPM：进入可疑；
- 可疑期间偏差降到450 RPM以内：取消；
- 450～600 RPM：保持当前可疑状态，避免门限附近反复跳变；
- 同一方向持续达到5秒：锁存影子候选；
- 偏差方向由低变高或由高变低时重新计时。

### 5.2 测速丢失

完成稳定等待后，测速连续无效5秒，锁存`TACH_LOST`影子候选。

即使现有控制器已经把PWM切到100%保护，诊断仍使用最后一个正常占空比作为参考，不会被100%保护掩盖。

## 6. 状态机

```text
DISABLED
STARTUP_BOOST
SETTLING
NORMAL
SPEED_SUSPECT
TACH_SUSPECT
SPEED_FAULT_SHADOW
TACH_FAULT_SHADOW
```

影子故障类型：

```text
NONE
SPEED_LOW
SPEED_HIGH
TACH_LOST
```

风机明确关闭后，影子锁存和本次诊断历史清除。

## 7. 调试器观察

调用：

```c
AppFanHealth_GetSnapshot()
```

重点字段：

```text
state
fault_type
fault_latched
monitoring_active
applied_duty_x100
reference_duty_x100
expected_rpm
actual_rpm
deviation_rpm
absolute_deviation_rpm
maximum_absolute_deviation_rpm
abnormal_elapsed_ms
settling_remaining_ms
suspect_count
shadow_fault_count
```

本阶段这些字段未加入W2查询。

## 8. 测试结果

新增host测试和AddressSanitizer/UndefinedBehaviorSanitizer通过：

```text
auto fan profile tests passed
fan health shadow phase-1 tests passed
fan health step-18 phase-1 host verification passed
```

覆盖：

- PWM→RPM反向插值；
- 启动100%加力排除；
- boost结束后5秒稳定等待；
- 700 RPM偏低连续5秒候选；
- 明确关闭清除影子锁存；
- 首次正常PWM的3秒等待；
- 测速丢失连续5秒候选；
- 控制器切到100%后仍保留最后正常参考占空比；
- Master/Slave严格警告编译；
- sanitizer检查。

以下回归测试全部通过：

```text
fan auto step-15
fan BPF stage-3
manual fan phase-1
manual fan phase-2/W2
```

## 9. 尚未实现

本阶段故意未实现：

- 候选确认后关闭风机；
- 全局故障锁；
- 拒绝AUTO/手动重新启动；
- 清除故障W2命令；
- 对象0x06诊断查询；
- 上位机故障详情和清除按钮。

## 10. 实机验证建议

先烧录step18，在不改变现有控制行为的前提下观察影子字段：

1. 10%、15%、20%、30%、40%、50%、60%、80%各稳定运行至少20秒；
2. 手动PWM从10%跳到50%、从80%跳到20%；
3. 手动目标转速1000、1500、1800、2100、2300 RPM；
4. AUTO低温到高温目标跳变；
5. 启动100%保持5秒及降速惯性；
6. 短暂遮挡FG、断开FG超过5秒；
7. 人为增加风阻，观察偏差方向和持续时间。

确认正常工况`shadow_fault_count`保持0后，再进入Phase 2正式停机锁存。
