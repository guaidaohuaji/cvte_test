# Step19 Phase 2：风机异常正式停机锁存报告

## 1. 本阶段目标

在Step18影子诊断通过主机测试的基础上，将以下异常升级为正式安全动作：

- 实际转速与PWM标定表预期值偏差绝对值大于600 RPM，并连续持续5000 ms；
- 风机正常运行阶段测速持续无效5000 ms。

确认故障后，固件必须：

1. 锁存故障类型和故障现场；
2. 立即将风机PWM强制为0%；
3. 阻止AUTO、手动PWM、手动目标转速和无测速100%保护重新启动风机；
4. 清故障后仍保持关闭；
5. 只有明确的新手动命令或重新发送AUTO模式命令才允许重启；
6. 不影响AUTO风门控制。

本阶段尚未修改W2对象0x06和上位机。

## 2. 判断参数

```text
偏差进入门限：       600 RPM
偏差恢复门限：       450 RPM
连续确认时间：       5000 ms
启动boost期间：      不检测
boost结束后等待：    5000 ms
一次PWM变化>=5%：    等待3000 ms
有效标定范围：       10%～80%
```

使用实际输出PWM而不是目标PWM。PWM到预期RPM仍使用同一张标定表的分段线性插值：

```text
10%→1000 RPM
15%→1200 RPM
20%→1400 RPM
30%→1700 RPM
40%→1900 RPM
50%→2100 RPM
60%→2200 RPM
80%→2300 RPM
```

## 3. 新增正式安全锁

`app_fan.c`新增底层安全接口：

```c
AppFan_TripSafetyFault();
AppFan_ClearSafetyFault();
AppFan_AuthorizeRestart();
AppFan_IsSafetyFaultLatched();
AppFan_IsRestartInhibited();
```

### 故障确认

```text
AppFanHealth确认故障
→ 保存故障时PWM/预期RPM/实际RPM/偏差
→ AppFan_TripSafetyFault()
→ TIM10_CH1比较值写0
→ enabled=false
→ target_duty=0
→ applied_duty=0
→ state=APP_FAN_STATE_SAFETY_LOCKED
```

`AppFan_SetEnabled(true, ...)`在以下任一条件下直接拒绝：

```text
safety_fault_latched = true
restart_inhibited = true
```

因此即使AUTO或手动控制器在后续循环继续尝试写PWM，也无法重新开启风机。

## 4. 清故障和重启语义

`AppFanHealth_ClearFault()`会：

```text
清除健康故障锁存
保持PWM=0
保持风机关闭
设置restart_inhibited=true
清除旧控制目标和测速历史
```

清故障本身不会恢复历史PWM、历史目标RPM或AUTO输出。

明确重启授权来自：

- 新的手动PWM命令；
- 新的手动目标转速命令；
- 明确重新发送AUTO模式命令。

AUTO原本已经处于AUTO模式时，重新发送AUTO也会被识别为操作员重启确认，并重新执行前馈启动流程。

## 5. 控制器隔离

### 手动控制

新增状态：

```text
APP_MANUAL_FAN_CTRL_SAFETY_LOCKED = 9
```

锁存或清故障后的重启禁止期间：

- 清除手动历史模式；
- 清除目标RPM和误差确认；
- 不再发送任何PWM命令；
- 新手动命令在故障未清除时返回失败；
- 故障清除后，新手动命令先授权重启再启动。

### AUTO控制

新增状态：

```text
APP_AUTO_FAN_CTRL_SAFETY_LOCKED = 9
```

风机安全锁期间，AUTO风机控制不再发送PWM命令，但温度评估和风门控制继续运行。这样不会因为风机故障连带关闭风门自动功能。

## 6. 诊断快照

`AppFanHealthSnapshot`新增或调整的关键字段：

```text
fault_latched
fault_type
shutdown_succeeded
restart_inhibited

fault_applied_duty_x100
fault_expected_rpm
fault_actual_rpm
fault_deviation_rpm

fault_count
shutdown_count
clear_count
```

故障类型：

```text
APP_FAN_HEALTH_FAULT_SPEED_LOW
APP_FAN_HEALTH_FAULT_SPEED_HIGH
APP_FAN_HEALTH_FAULT_TACH_LOST
```

状态：

```text
APP_FAN_HEALTH_STATE_SPEED_FAULT_LATCHED
APP_FAN_HEALTH_STATE_TACH_FAULT_LATCHED
```

## 7. 测速丢失与100%保护兼容

AUTO和手动定速原有逻辑会在测速丢失时请求100%保护。健康模块仍保留最后一次10%～80%正常PWM作为参考：

```text
正常PWM=45%
→ 测速丢失
→ 控制器请求100%
→ 健康诊断继续按45%预期约2000 RPM累计无测速时间
→ 连续5秒后锁存测速丢失故障
→ 强制0%
```

因此100%保护不会掩盖测速丢失故障。

## 8. 测试结果

主机测试通过：

```text
fan startup and safety-lock tests passed
fan health formal shutdown phase-2 tests passed
auto fan closed-loop and safety-lock tests passed
manual fan RPM controller phase-1 tests passed
app_uart fan manual RPM protocol tests passed
fan BPF stage-3 host verification passed
```

AddressSanitizer和UndefinedBehaviorSanitizer通过：

```text
app_fan safety lock ASan/UBSan: passed
app_fan_health ASan/UBSan: passed
```

覆盖内容：

- 600 RPM/5000 ms正式故障确认；
- 转速偏低故障；
- 测速丢失故障；
- 故障现场冻结；
- 立即强制0%；
- 底层拒绝重新enable；
- 风机关闭不会自动清除故障；
- 清故障后仍禁止重启；
- 显式授权后才能启动；
- AUTO锁存期间不发送PWM；
- 手动控制清除历史目标；
- 原AUTO、手动定速、W2和BPF回归测试。

## 9. 当前限制

1. 当前没有W2清故障命令。目标板上故障后只能掉电复位，或由调试器/后续协议调用`AppFanHealth_ClearFault()`。
2. 当前对象0x06扩展V1没有故障类型、故障现场和锁存状态字段。
3. 当前上位机无法显示详细故障，也没有“清除风机故障”按钮。
4. 尚未使用ARM交叉编译器构建Master/Slave固件。
5. 尚未完成真实风机误报和故障停机验证。

## 10. 下一阶段

下一阶段建议扩展W2对象0x06：

- 增加清故障控制子命令；
- 增加扩展查询V2；
- 返回故障锁存、故障类型、预期RPM、故障RPM、故障PWM、偏差、异常累计时间和重启禁止状态；
- 保留旧18字节查询和扩展V1兼容；
- 在上位机增加故障详情和清故障按钮。
