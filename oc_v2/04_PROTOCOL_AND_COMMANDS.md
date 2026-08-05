# USART1/W2与单总线协议

## 1. W2帧

```text
7E | TYPE | LENGTH | DATA | CHECKSUM
```

- TYPE：`A1`查询，`A2`控制
- `LENGTH = DATA长度 + 2`
- CHECKSUM：`TYPE + LENGTH + DATA`低8位
- 帧头`7E`不参与校验
- 多字节字段小端
- USART1：9600、8N1、HEX、不加CR/LF

## 2. 对象表

| 对象 | 名称 | A1 | A2 |
|---:|---|---|---|
| 0x01 | 通用PWM输出 | 支持 | 支持 |
| 0x02 | PE8 LED | 支持 | 支持 |
| 0x03 | PWM输入 | 支持 | 只读 |
| 0x04 | PD13输出 | 支持 | 支持 |
| 0x05 | NTC | 支持 | 只读 |
| 0x06 | 风机 | 支持 | 支持 |
| 0x07 | 风门 | 支持 | Master可控，Slave只读 |
| 0x08 | 单总线 | 支持 | Master可控，Slave只读 |

## 3. A1查询命令

```text
0x01: 7E A1 03 01 A5
0x02: 7E A1 03 02 A6
0x03: 7E A1 03 03 A7
0x04: 7E A1 03 04 A8
0x05: 7E A1 03 05 A9
0x06: 7E A1 03 06 AA
0x07: 7E A1 03 07 AB
0x08: 7E A1 03 08 AC
```

## 4. A1响应布局

### 对象0x01：9字节DATA

```text
[0] status
[1] object
[2] enabled
[3..6] actual_frequency_hz, LE32
[7..8] duty_x100, LE16
```

禁用时查询返回的是目标占空比，不是实际CCR=0占空比。

### 对象0x02：4字节DATA

```text
[0] status
[1] object
[2] mode: 0=AUTO, 1=MANUAL
[3] actual_led_on
```

### 对象0x03：11字节DATA

```text
[0] status
[1] object
[2] input_status
[3..6] frequency_millihz, LE32
[7..8] duty_x100, LE16
[9..10] age_ms, LE16
```

非`OK`状态下，W2将频率和占空比编码为0。

### 对象0x04：3字节DATA

```text
[0] status
[1] object
[2] PD13 ODR level
```

### 对象0x05：15字节DATA

```text
[0] status
[1] object
[2] ntc_state
[3..4] adc_raw, LE16
[5..6] voltage_mv, LE16
[7..10] resistance_ohm, LE32
[11..12] temp_centi_c, signed LE16
[13..14] age_ms, LE16
```

### 对象0x06：兼容查询与扩展查询

#### 旧查询：18字节DATA，保持不变

A1 DATA：`06`

```text
[0] status
[1] object
[2] fan_state
[3] enabled
[4..5] target_duty_x100, LE16
[6..7] applied_duty_x100, LE16
[8..9] pwm_frequency_hz, LE16
[10..13] fg_frequency_millihz, LE32
[14..15] rpm, LE16
[16..17] tach_age_ms, LE16
```

旧查询帧：`7E A1 03 06 AA`

#### 扩展查询V1：32字节DATA

A1 DATA：`06 01`

查询帧：`7E A1 04 06 01 AC`

```text
[0] status
[1] object = 0x06
[2] schema_version = 0x01
[3] manual_control_mode
    0=OFF, 1=DUTY, 2=SPEED
[4] manual_control_state
    0=INACTIVE, 1=STARTING, 2=WAIT_TACH, 3=ADJUSTING,
    4=IN_TOLERANCE, 5=TACH_FAULT, 6=SATURATED_LOW,
    7=SATURATED_HIGH, 8=HW_ERROR, 9=SAFETY_LOCKED
[5] flags
    bit0 fan_enabled
    bit1 fan_tach_valid
    bit2 manual_tach_valid
    bit3 manual_in_tolerance
    bit4 global_auto_active
    bit5 startup_boost_active
    bit6 tach_fault_active
    bit7 hardware_config_or_safety_fault
[6..7] target_rpm, LE16
[8..9] rpm_error, signed LE16；定义为 target_rpm - actual_rpm
[10..11] feedforward_duty_x100, LE16
[12..13] fan_target_duty_x100, LE16
[14..15] fan_applied_duty_x100, LE16
[16..17] pwm_frequency_hz, LE16
[18..21] fg_frequency_millihz, LE32
[22..23] actual_rpm, LE16
[24..25] tach_age_ms, LE16
[26..27] adjust_count, 饱和LE16
[28..29] fault_count, 饱和LE16
[30] global_auto_mode, 0=MANUAL, 1=AUTO
[31] reserved = 0
```

只有长度恰好为2且选择器为`01`时返回扩展V1；其他带尾部字节的旧式查询继续返回18字节旧格式，以保持历史兼容性。


#### 风机健康诊断查询V2：32字节DATA

A1 DATA：`06 02`

查询帧：`7E A1 04 06 02 AD`

该响应是独立的健康诊断快照，不替代旧18字节查询或扩展V1。

```text
[0] status
[1] object = 0x06
[2] schema_version = 0x02
[3] health_state
    0=DISABLED
    1=STARTUP_BOOST
    2=SETTLING
    3=NORMAL
    4=SPEED_SUSPECT
    5=TACH_SUSPECT
    6=SPEED_FAULT_LATCHED
    7=TACH_FAULT_LATCHED
[4] fault_type
    0=NONE
    1=SPEED_LOW
    2=SPEED_HIGH
    3=TACH_LOST
[5] flags
    bit0 fault_latched
    bit1 monitoring_active
    bit2 tach_valid
    bit3 shutdown_succeeded
    bit4 restart_inhibited
    bit5 fan_enabled
    bit6 fan_state_is_safety_locked
    bit7 reserved
[6..7]   applied_duty_x100, LE16
[8..9]   reference_duty_x100, LE16
[10..11] expected_rpm, LE16
[12..13] actual_rpm, LE16
[14..15] deviation_rpm, signed LE16；定义为 actual_rpm - expected_rpm
[16..17] absolute_deviation_rpm, LE16
[18..19] maximum_absolute_deviation_rpm, LE16
[20..21] fault_applied_duty_x100, LE16
[22..23] fault_expected_rpm, LE16
[24..25] fault_actual_rpm, LE16
[26..27] fault_deviation_rpm, signed LE16
[28..29] abnormal_elapsed_ms, 饱和LE16
[30..31] settling_remaining_ms, 饱和LE16
```

只有长度恰好为2且选择器为`02`时返回健康诊断V2。旧固件会把`06 02`当作旧式查询并返回18字节DATA，因此上位机必须同时校验DATA长度和`schema_version=0x02`。

### 对象0x07：23字节DATA

A1 查询: `7E A1 03 07 AB`

```text
[0] status
[1] object = 0x07
[2] damper_state
   0x00 UNINITIALIZED  0x01 POSITION_UNKNOWN  0x02 MOVING_FORWARD
   0x03 MOVING_REVERSE  0x04 IDLE_RELEASED    0x05 POST_MOVE_HOLD
   0x06 STOPPED         0x07 FAULT            0x08 BOOT_HOMING
   0xFE UNAVAILABLE (Slave)
[3] flags
   bit0 position_valid   bit1 moving   bit2 direction (forward=1)
   bit3 released         bit4 position_is_estimated
   bit5 boot_homing_active   bit6-7 reserved
[4..7]   current_steps, int32 LE
[8..11]  target_steps, int32 LE
[12..15] remaining_steps, uint32 LE
[16..17] full_travel_steps, LE16 (当前1850=0x073A)
[18..19] configured_pps, LE16 (当前100=0x0064)
[20] last_command (0x00=NONE, 0x01=ABS, 0x02=REL, 0x03=BOOT_HOMING)
[21] last_result  (0x00=SUCCESS, 0x01=ABRT_STOP, 0x02=ABRT_RELEASE,
                   0x03=ABRT_EMERG, 0x04=HW_ERROR, 0x05=IN_PROGRESS)
[22] fault_flags (bit0=emergency_shutdown, bit1=internal_range)
```

Step24 Master上电后不等待人工位置确认，立即沿打开方向运行1700步。运行期间状态0x08、位置无效；完成后建立0步=全开参考。对象0x07帧长度不变。

Slave A1: status=OK, damper_state=UNAVAILABLE(0xFE), flags=0x00, 位置/命令/结果清零,
full_travel_steps 和 configured_pps 返回编译配置值.

A2 控制 (全部严格长度):

MOVE_ABSOLUTE (len=6):
DATA: 07 01 target_int32_LE

MOVE_RELATIVE (len=6):
DATA: 07 02 delta_int32_LE

STOP (len=2):
DATA: 07 03

RELEASE (len=2):
DATA: 07 04

SET_CURRENT_POSITION (len=6):
DATA: 07 05 position_int32_LE

A2 成功响应: `7E A2 04 00 07 AD`
A2 OK 仅表示命令已接受; 运动结果通过 A1 中的 last_result 查询.
运动中 MOVE → BUSY: `7E A2 04 09 07 B6`
Slave A2 → READ_ONLY: `7E A2 04 07 07 B4`

data[0]=对象ID, data[1]=子命令.
frame_buf 不保存帧头 0x7E.

### 对象0x08

Step25保持16字节A1响应长度，但启用原保留字段：

```text
status, object, role, link_state, busy, pending,
last_operation, result_code, address, value,
slave_address, context_valid, last_response_age_ms
```

A1旧查询`08`默认查看0x02；新查询`08 slave_address`查看指定从机上下文，
只读主机内存，不会在USART6上发送握手或保活帧。

A2旧格式长度6默认访问0x02：

```text
08 | operation | address_LE16 | value_LE16
```

A2新格式长度7显式指定从机：

```text
08 | slave_address | operation | address_LE16 | value_LE16
```

主机上电不自动握手。READ/WRITE提交后，若该从机最近有效响应年龄小于450 ms则直接操作；
否则先完成两次握手，再自动继续原操作。没有后台扫描或保活。
从机地址0x02～0xFE，每个从机只响应目标地址等于本机编译地址的帧。

## 5. A2控制布局

### 对象0x01

```text
DATA:
01 | enable | frequency_hz LE32 | duty_x100 LE16
```

- enable=1：频率1～840000，占空比0～10000；固件按最接近的1%量化
- enable=0：频率和占空比字段必须全0

### 对象0x02

```text
02 | cmd
cmd=0 手动灭
cmd=1 手动亮
cmd=2 自动模式
```

### 对象0x04

```text
04 | level
level=0 PD13低
level=1 PD13高
```

### 对象0x06

统一布局：

```text
06 | mode | value LE16
```

模式：

```text
mode=0 OFF
  value必须为0
  示例：7E A2 06 06 00 00 00 AE

mode=1 DUTY
  value=duty_x100
  value=0保持历史特殊语义：enabled=true但输出0，状态OFF
  非零范围1000～10000，即10.00%～100.00%
  30.00%示例：7E A2 06 06 01 B8 0B 72

mode=2 SPEED
  value=target_rpm
  范围1000～2300 RPM
  1500 RPM示例：7E A2 06 06 02 DC 05 91

mode=3 CLEAR_FAULT
  value必须为0
  DATA长度必须严格等于4，不接受尾部字节
  MANUAL和AUTO模式下均允许执行
  示例：7E A2 06 06 03 00 00 B1
```

成功响应仍为：`7E A2 04 00 06 AC`。

`mode=0/1`与旧协议中的`enable=0/1`字节值完全一致，因此旧上位机的关闭和占空比命令不需要修改。`mode=0/1/2`通过`AppManualFanControl`执行，并在全局AUTO模式下返回`STATUS_MODE_LOCKED(0x0B)`。`mode=3`直接调用风机健康模块清除故障锁存，在MANUAL和AUTO均允许。

清除故障后风机仍保持0%，`restart_inhibited`仍为1。之后必须发送新的手动DUTY/SPEED命令，或重新发送AUTO模式命令，才会授权重启。没有故障时执行清除命令按幂等操作返回OK。

### 对象0x08

```text
08 | operation | address LE16 | value LE16
```

操作：

```text
01 强制重新握手
03 写
06 读
```

## 6. 现有长度兼容性

对象`0x01`已改为严格`len == 8`，不再接受尾部额外DATA字节。对象`0x02、0x04`以及对象`0x06`的OFF/DUTY/SPEED控制仍保留既有最低长度兼容行为；对象`0x06 mode=3 CLEAR_FAULT`使用严格`len == 4`。

对象`0x06`查询支持精确选择器`06 01`（扩展V1）和`06 02`（健康诊断V2）；除这两个精确组合外，历史尾部字节仍按旧18字节查询处理。

对象`0x08`使用严格`len == 6`。

后续不得把兼容对象直接改成严格长度而不评估现有上位机兼容性。

## 7. 状态枚举

### PWM输入

```text
0 SEARCH
1 OK
2 STATIC_HIGH
3 STATIC_LOW
4 OUT_OF_RANGE
5 UNSTABLE
6 HW_ERROR
```

当前实现主要实际赋值0～3。

### NTC

```text
0 SEARCH
1 OK
2 ADC_ERROR
3 OPEN_OR_UNDER_TEMP
4 SHORT_OR_OVER_TEMP
5 CALC_ERROR
6 CONFIG_ERROR
```

### 风机

```text
0 OFF
1 STARTUP_BOOST
2 RUNNING
3 NO_TACH
4 TACH_UNRELIABLE
5 PWM_ERROR
6 CONFIG_ERROR
7 SAFETY_LOCKED
```

### W2通用状态

```text
00 OK
01 LENGTH_ERROR
02 CHECKSUM_ERROR
03 UNSUPPORTED_TYPE
04 UNSUPPORTED_OBJECT
05 PARAM_RANGE
06 APPLY_FAILED
07 READ_ONLY
08 NO_VALID_DATA
09 BUSY
0A HW_ERROR
```

部分常量存在但解析器对坏校验帧当前直接丢弃，不一定回错误帧。

## 8. 单总线帧

```text
AA | S | D | L | DATA | XOR
```

握手和读写测试向量见`serial_test_vectors.txt`。
