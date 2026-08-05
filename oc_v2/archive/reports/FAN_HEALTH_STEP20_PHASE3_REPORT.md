# Step20 Phase 3：风机健康故障 W2 查询与清除协议报告

## 1. 本阶段目标

在 Step19 已实现的正式故障锁存、强制 0% 停机和重启禁止基础上，为 W2 对象 `0x06` 增加：

1. 独立的风机健康诊断查询 V2；
2. 明确的清除风机健康故障命令；
3. 对旧 18 字节查询和扩展查询 V1 的兼容；
4. 让旧版上位机至少能够把安全锁识别为故障；
5. 保持“清故障不等于重新启动”的安全语义。

本阶段只修改单片机程序和协议文档，尚未修改上位机。

## 2. 兼容性设计

对象 `0x06` 现在支持三种查询：

```text
A1 DATA = 06       → 旧 18 字节风机快照
A1 DATA = 06 01    → 扩展控制状态 V1，32 字节
A1 DATA = 06 02    → 风机健康诊断 V2，32 字节
```

查询帧：

```text
旧查询：       7E A1 03 06 AA
扩展 V1：      7E A1 04 06 01 AC
健康诊断 V2：  7E A1 04 06 02 AD
```

只有 DATA 长度恰好为 2 且选择器分别为 `01` 或 `02` 时，才返回对应的版本化响应。其他带尾部字节的历史查询仍返回旧 18 字节格式。

旧固件收到 `06 02` 时可能忽略选择器并返回 18 字节旧快照。因此新上位机必须同时检查：

```text
DATA 长度 == 32
且 data[2] == 0x02
```

不能只根据响应长度判断，因为扩展 V1 也是 32 字节。

## 3. 健康诊断查询 V2

### 请求

```text
7E A1 04 06 02 AD
```

### 响应 DATA 布局

固定 32 字节：

```text
[0] status
[1] object = 0x06
[2] schema_version = 0x02
[3] health_state
[4] fault_type
[5] flags
[6..7]   applied_duty_x100, LE16
[8..9]   reference_duty_x100, LE16
[10..11] expected_rpm, LE16
[12..13] actual_rpm, LE16
[14..15] deviation_rpm, signed LE16
[16..17] absolute_deviation_rpm, LE16
[18..19] maximum_absolute_deviation_rpm, LE16
[20..21] fault_applied_duty_x100, LE16
[22..23] fault_expected_rpm, LE16
[24..25] fault_actual_rpm, LE16
[26..27] fault_deviation_rpm, signed LE16
[28..29] abnormal_elapsed_ms, 饱和 LE16
[30..31] settling_remaining_ms, 饱和 LE16
```

偏差定义：

```text
deviation_rpm = actual_rpm - expected_rpm
```

负数表示实际转速明显偏低，正数表示实际转速明显偏高。

### health_state

```text
0 DISABLED
1 STARTUP_BOOST
2 SETTLING
3 NORMAL
4 SPEED_SUSPECT
5 TACH_SUSPECT
6 SPEED_FAULT_LATCHED
7 TACH_FAULT_LATCHED
```

### fault_type

```text
0 NONE
1 SPEED_LOW
2 SPEED_HIGH
3 TACH_LOST
```

### flags

```text
bit0 fault_latched
bit1 monitoring_active
bit2 tach_valid
bit3 shutdown_succeeded
bit4 restart_inhibited
bit5 fan_enabled
bit6 fan_state_is_safety_locked
bit7 reserved
```

### 故障现场字段

故障锁存后，即使实际 PWM 已经强制降为 0%，以下字段仍保留故障发生时的数据：

```text
fault_applied_duty_x100
fault_expected_rpm
fault_actual_rpm
fault_deviation_rpm
```

因此上位机可以同时显示当前停机状态和故障发生时的原始现场。

## 4. 清故障命令

对象 `0x06` 新增控制模式：

```text
06 | mode=03 | value=0000 LE16
```

完整帧：

```text
7E A2 06 06 03 00 00 B1
```

成功响应：

```text
7E A2 04 00 06 AC
```

规则：

- DATA 长度必须严格等于 4；
- value 必须为 0；
- MANUAL 和 AUTO 模式下均允许执行；
- 没有故障时执行为幂等成功；
- 底层清除失败时返回 `STATUS_HW_ERROR(0x0A)`。

## 5. 清故障后的安全语义

清故障仅清除故障锁存，不会恢复历史输出：

```text
清除 fault_latched
保持 PWM = 0%
保持 enabled = false
保持 restart_inhibited = true
```

之后必须出现明确的新操作才能授权重启：

- 发送新的手动 PWM 命令；
- 发送新的手动目标转速命令；
- 重新发送 AUTO 模式命令。

因此上位机的“清除风机故障”按钮不能同时发送启动命令。

## 6. AUTO 模式处理

普通手动命令仍遵循原规则：

```text
AUTO 模式下 OFF/DUTY/SPEED → STATUS_MODE_LOCKED
```

清故障命令是例外：

```text
AUTO 模式下 CLEAR_FAULT → 允许
```

原因是清故障不会驱动风机，也不会抢占 AUTO；清除后 AUTO 风机仍处于重启禁止状态，必须由操作员重新发送 AUTO 模式命令才能恢复。

## 7. 扩展 V1 的兼容增强

扩展 V1 的布局没有改变，但 `flags bit7` 现在也会在以下安全锁状态置位：

```text
APP_FAN_STATE_SAFETY_LOCKED
APP_MANUAL_FAN_CTRL_SAFETY_LOCKED
```

因此只支持扩展 V1 的旧上位机至少会把健康故障停机显示为“故障”，而不是普通未知状态。

扩展 V1 的 `manual_control_state` 还保留新增枚举：

```text
9 = SAFETY_LOCKED
```

## 8. 测试覆盖

新增或扩展测试覆盖：

- 旧 18 字节查询保持不变；
- 扩展 V1 32 字节布局保持不变；
- 扩展 V1 在安全锁时置位故障标志；
- 健康诊断 V2 32 字节完整布局；
- signed LE16 负偏差编码；
- 计时字段饱和到 65535；
- 健康快照读取失败映射为 `HW_ERROR`；
- MANUAL 模式清故障；
- AUTO 模式清故障；
- 清故障 value 非零拒绝；
- 清故障尾部字节拒绝；
- 底层清除失败映射为 `HW_ERROR`；
- Master/Slave 条件编译；
- AddressSanitizer 和 UndefinedBehaviorSanitizer；
- Step19 正式停机、安全锁、AUTO、手动定速和 BPF 回归。

测试结果：

```text
fan health step-20 phase-3 W2 verification passed
manual fan RPM controller phase-2 host verification passed
fan BPF stage-3 host verification passed
ASan/UBSan completed without diagnostics
```

## 9. 文档同步

已同步更新：

```text
oc_v2/04_PROTOCOL_AND_COMMANDS.md
oc_v2/05_CURRENT_STATUS_AND_EVIDENCE.md
oc_v2/07_KNOWN_RISKS_AND_BACKLOG.md
oc_v2/10_EXISTING_APPLICATION_FEATURES.md
oc_v2/11_EXISTING_FEATURE_TEST_COMMANDS.md
oc_v2/project_state.yaml
oc_v2/serial_test_vectors.txt
oc_v2/manifest.json
oc_v2.zip
../../reference/protocol/STM32_W2_step20_通讯协议_OpenCode生成解析规范.txt
```

## 10. 当前限制与下一阶段

当前环境没有 `arm-none-eabi-gcc`，因此没有生成新的 HEX/BIN，也没有完成目标板串口联调。

下一阶段需要修改上位机：

- 轮询 `06 02` 健康诊断 V2；
- 显示正常、待稳定、异常确认中和故障锁存；
- 显示故障类型、预期转速、故障转速、故障 PWM 和偏差；
- 增加“清除风机故障”按钮；
- 故障锁存时禁用启动类按钮；
- 清除后仍保持启动按钮可用但不自动启动；
- 对旧固件 18 字节回退保持兼容。
