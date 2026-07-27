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
    7=SATURATED_HIGH, 8=HW_ERROR
[5] flags
    bit0 fan_enabled
    bit1 fan_tach_valid
    bit2 manual_tach_valid
    bit3 manual_in_tolerance
    bit4 global_auto_active
    bit5 startup_boost_active
    bit6 tach_fault_active
    bit7 hardware_or_config_error
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

### 对象0x07：23字节DATA

A1 查询: `7E A1 03 07 AB`

```text
[0] status
[1] object = 0x07
[2] damper_state
   0x00 UNINITIALIZED  0x01 POSITION_UNKNOWN  0x02 MOVING_FORWARD
   0x03 MOVING_REVERSE  0x04 IDLE_RELEASED    0x05 POST_MOVE_HOLD
   0x06 STOPPED         0x07 FAULT            0xFE UNAVAILABLE (Slave)
[3] flags
   bit0 position_valid   bit1 moving   bit2 direction (forward=1)
   bit3 released         bit4 position_is_estimated   bit5-7 reserved
[4..7]   current_steps, int32 LE
[8..11]  target_steps, int32 LE
[12..15] remaining_steps, uint32 LE
[16..17] full_travel_steps, LE16 (当前1850=0x073A)
[18..19] configured_pps, LE16 (当前100=0x0064)
[20] last_command (0x00=NONE, 0x01=ABS, 0x02=REL)
[21] last_result  (0x00=SUCCESS, 0x01=ABRT_STOP, 0x02=ABRT_RELEASE,
                   0x03=ABRT_EMERG, 0x04=HW_ERROR, 0x05=IN_PROGRESS)
[22] fault_flags (bit0=emergency_shutdown, bit1=internal_range)
```

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

保持V1交接定义：

```text
status, object, role, link_state, busy, pending,
last_operation, result_code, address, value, reserved
```

## 5. A2控制布局

### 对象0x01

```text
DATA:
01 | enable | frequency_hz LE32 | duty_x100 LE16
```

- enable=1：频率1～100000，占空比0～10000
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
```

成功响应仍为：`7E A2 04 00 06 AC`。

`mode=0/1`与旧协议中的`enable=0/1`字节值完全一致，因此旧上位机的关闭和占空比命令不需要修改。所有模式均通过`AppManualFanControl`执行；全局AUTO模式下返回`STATUS_MODE_LOCKED(0x0B)`。

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

源码中对象`0x01、0x02、0x04、0x06`控制仍使用“`len < 最低长度`”检查，因此继续接受尾部额外DATA字节。

对象`0x06`查询新增精确的`06 01`扩展选择器；除该组合外，历史尾部字节仍按旧18字节查询处理。

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
