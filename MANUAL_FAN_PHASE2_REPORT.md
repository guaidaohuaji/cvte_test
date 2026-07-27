# 手动风机目标转速控制 Phase 2 报告

## 1. 阶段目标

本阶段在 step16 的单片机内部手动转速控制器基础上，完成 W2 对象 `0x06` 的协议接入。

本阶段不修改上位机。现有上位机仍可继续使用原来的关闭和占空比控制命令。

## 2. 源码审计结论

step16 已具备：

- `AppManualFanControl_SetOff()`；
- `AppManualFanControl_SetDuty()`；
- `AppManualFanControl_SetTargetRpm()`；
- 手动目标转速前馈和闭环；
- MANUAL/AUTO 所有权隔离；
- 100%/5秒启动加力；
- 1000～2300 RPM 目标范围；
- 10%～80%正常闭环范围。

但 `app_uart.c` 的对象 `0x06` 仍然直接调用 `AppFan_SetEnabled()`，因此存在两个问题：

1. 上位机不能发送目标RPM；
2. 旧占空比命令不会更新 `AppManualFanControl` 的模式快照。

本阶段将对象 `0x06` 统一路由至 `AppManualFanControl`。

## 3. A2控制协议

DATA 固定前4字节语义：

```text
06 | mode | value_le16
```

### 3.1 关闭

```text
mode = 0
value = 0
```

示例：

```text
7E A2 06 06 00 00 00 AE
```

### 3.2 手动占空比

```text
mode = 1
value = duty_x100
```

`mode=1` 与旧协议的 `enable=1` 数值完全相同，因此旧上位机无需修改。

合法值：

```text
0
1000～10000
```

30.00% 示例：

```text
7E A2 06 06 01 B8 0B 72
```

### 3.3 手动目标转速

```text
mode = 2
value = target_rpm
```

合法范围：

```text
1000～2300 RPM
```

1500 RPM 示例：

```text
7E A2 06 06 02 DC 05 91
```

### 3.4 响应

成功响应保持不变：

```text
7E A2 04 00 06 AC
```

状态映射：

```text
OK             -> 0x00
长度错误       -> 0x01
参数越界       -> 0x05
硬件错误       -> 0x0A
AUTO模式锁定   -> 0x0B
```

全局 AUTO 模式拥有最高优先级。AUTO 下收到任何对象 `0x06` 手动控制命令时，先返回 `MODE_LOCKED`，不会调用手动控制器。

## 4. A1查询兼容扩展

### 4.1 旧查询保持不变

查询：

```text
7E A1 03 06 AA
```

返回原有18字节 DATA：

```text
status
object
fan_state
enabled
target_duty_x100
applied_duty_x100
pwm_frequency_hz
fg_frequency_millihz
rpm
tach_age_ms
```

### 4.2 扩展查询V1

查询：

```text
7E A1 04 06 01 AC
```

返回32字节 DATA：

```text
[0] status
[1] object
[2] schema_version = 1
[3] manual_control_mode
[4] manual_control_state
[5] flags
[6..7] target_rpm
[8..9] rpm_error，signed LE16
[10..11] feedforward_duty_x100
[12..13] fan_target_duty_x100
[14..15] fan_applied_duty_x100
[16..17] pwm_frequency_hz
[18..21] fg_frequency_millihz
[22..23] actual_rpm
[24..25] tach_age_ms
[26..27] adjust_count，饱和LE16
[28..29] fault_count，饱和LE16
[30] global_auto_mode
[31] reserved
```

flags：

```text
bit0 fan_enabled
bit1 fan_tach_valid
bit2 manual_tach_valid
bit3 manual_in_tolerance
bit4 global_auto_active
bit5 startup_boost_active
bit6 tach_fault_active
bit7 hardware_or_config_error
```

只有 DATA 恰好为 `06 01` 时返回扩展格式。历史固件允许对象 `0x06` 查询携带尾部字节，因此其他带尾部字节的查询仍返回旧18字节格式。

## 5. 兼容性保护

本阶段保留了以下行为：

- 旧 `mode=0/1` 控制字节数值不变；
- 旧成功响应不变；
- 旧18字节查询不变；
- 控制仍采用最小长度检查，继续容忍历史尾部字节；
- `mode=1,value=0` 的历史特殊语义不变；
- AUTO 模式仍拒绝手动风机命令；
- 对象 `0x01～0x05、0x07～0x09` 未改协议；
- 单总线、风门、NTC、LED和BPF测速逻辑未改。

## 6. 修改文件

核心修改：

```text
Core/Src/app_uart.c
```

新增测试：

```text
Tests/test_app_uart_fan_manual.c
Tests/run_manual_fan_phase2_host_test.sh
```

同步文档：

```text
oc_v2/04_PROTOCOL_AND_COMMANDS.md
oc_v2/10_EXISTING_APPLICATION_FEATURES.md
oc_v2/03_SOFTWARE_ARCHITECTURE.md
oc_v2/09_SOURCE_INDEX.md
oc_v2/11_EXISTING_FEATURE_TEST_COMMANDS.md
oc_v2/project_state.yaml
oc_v2/serial_test_vectors.txt
oc_v2/manifest.json
oc_v2.zip
```

## 7. 测试

Phase 2协议测试覆盖：

- 旧查询返回布局不变；
- 带非扩展尾部字节的旧查询仍返回旧格式；
- 扩展V1的32字节布局；
- signed RPM误差编码；
- 32位计数饱和到16位；
- 旧PWM命令路由到手动控制器；
- 新RPM命令路由到目标转速接口；
- OFF参数必须为0；
- 参数、硬件和模式锁定状态映射；
- AUTO优先锁定；
- 控制尾部字节兼容；
- Master和Slave条件编译。

回归测试重新运行：

- Phase 1手动转速控制器；
- step15自动风机控制；
- BPF测速和正式源仲裁。

结果：

```text
app_uart fan manual RPM protocol tests passed
manual fan RPM controller phase-1 host verification passed
fan auto step-15 host verification passed
fan BPF stage-3 host verification passed
manual fan RPM controller phase-2 host verification passed
```

AddressSanitizer和UndefinedBehaviorSanitizer测试通过。

## 8. 尚未完成

当前运行环境没有 `arm-none-eabi-gcc` 和 PowerShell，因此没有生成新的HEX/BIN，也没有完成目标板编译。

本阶段尚未修改上位机。下一阶段应在 v0.1.3 上位机中实现：

- PWM/目标转速控制方式选择；
- 1000～2300 RPM输入；
- `mode=2`命令编码；
- 扩展查询V1解码；
- 目标RPM、实际RPM、误差和控制阶段显示；
- 旧固件只支持18字节查询时的自动兼容回退。
