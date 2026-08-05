# 既有功能串口测试命令

设置：

```text
USART1
9600
8N1
HEX
不加回车换行
```

## 1. 查询

```text
PWM输出: 7E A1 03 01 A5
LED:     7E A1 03 02 A6
PWM输入: 7E A1 03 03 A7
PD13:    7E A1 03 04 A8
NTC:     7E A1 03 05 A9
风机旧查询:      7E A1 03 06 AA
风机扩展查询V1:  7E A1 04 06 01 AC
风门:    7E A1 03 07 AB
单总线:  7E A1 03 08 AC
```

## 2. 通用PWM输出

### 1 kHz、50.00%、enable

```text
7E A2 0A 01 01 E8 03 00 00 88 13 34
```

成功响应：

```text
7E A2 04 00 01 A7
```

PB6预期：

```text
约1 kHz
约50%
```

### 100 kHz、1%（重点回归）

```text
7E A2 0A 01 01 A0 86 01 00 64 00 39
```

理论寄存器：PSC=0，ARR=839，CCR=8；实际占空比约0.95%。

### 840 kHz、1%（软件范围上限）

```text
7E A2 0A 01 01 40 D1 0C 00 64 00 2F
```

理论周期计数100、CCR=1。该点需要示波器实测引脚脉冲质量。

### disable

```text
7E A2 0A 01 00 00 00 00 00 00 00 AD
```

成功响应同上。PB6应保持非激活电平。

## 3. LED

```text
手动灭: 7E A2 04 02 00 A8
手动亮: 7E A2 04 02 01 A9
自动:   7E A2 04 02 02 AA
```

成功响应：

```text
7E A2 04 00 02 A8
```

## 4. PD13

```text
低: 7E A2 04 04 00 AA
高: 7E A2 04 04 01 AB
```

成功响应：

```text
7E A2 04 00 04 AA
```

随后查询：

```text
7E A1 03 04 A8
```

## 5. 风机

### 手动占空比100%

```text
7E A2 06 06 01 10 27 E6
```

成功响应：

```text
7E A2 04 00 06 AC
```

预期：

```text
立即STARTUP_BOOST
PB8约10 kHz/100%
持续5秒后切换目标占空比
```

### 手动占空比50%

```text
7E A2 06 06 01 88 13 4A
```

首次非零启动先100% boost 5秒，之后50%。

### 手动目标1500 RPM

```text
7E A2 06 06 02 DC 05 91
```

预期：

```text
未运行时先100% boost 5秒
之后切换到查表前馈PWM（约23%）
等待有效测速后进行1%/2%/3%闭环修正
```

使用扩展查询：

```text
7E A1 04 06 01 AC
```

重点检查：

```text
manual_control_mode = 2
manual_control_state
目标RPM = 1500
实际RPM
rpm_error
in_tolerance
```

### 关闭

```text
7E A2 06 06 00 00 00 AE
```

### AUTO模式锁定

全局AUTO下发送任意对象0x06控制，预期：

```text
7E A2 04 0B 06 B7
```

## 6. PWM输入建议测试矩阵

信号源接PE9，共地。

| 输入 | 期望 |
|---|---|
| 1 kHz, 50% | 频率约1,000,000 mHz；占空比约5000 |
| 1 kHz, 25% | 用于确认是否报告2500或7500 |
| 1 kHz, 75% | 用于确认是否报告7500或2500 |
| 10 Hz | 范围下部 |
| 5 kHz | 自动量程中频边界附近 |
| 静态高 >2.5 s | STATIC_HIGH |
| 静态低 >2.5 s | STATIC_LOW |

25%和75%是确认当前占空比方向的关键用例。

Step23还应逐级验证20 kHz、50 kHz、100 kHz、150 kHz、200 kHz，并在100 Hz与5 kHz附近观察量程切换。

## 7. NTC建议测试

查询：

```text
7E A1 03 05 A9
```

记录：

```text
state
adc_raw
voltage_mv
resistance_ohm
temp_centi_c
age_ms
```

至少使用三个已知点：

- 低温/高阻；
- 中间点；
- 高温/低阻。

不要仅凭`state=OK`判断准确。

## 8. 风机反馈建议测试

查询：

```text
7E A1 03 06 AA
```

记录：

```text
fan_state
target duty
applied duty
FG frequency
RPM
tach age
```

使用示波器同时观察：

- PB8风机PWM；
- PA0反馈电压；
- 实际转速仪。

验证：

```text
RPM ≈ FG_Hz × 30
```

若风机不是2脉冲/转，该公式会错误。

## 9. 只读对象控制

对对象0x03和0x05发送A2，应返回READ_ONLY：

```text
status=07
```

不要把查询帧当作控制帧。

## 10. 风门 (对象0x07, 当前 100 PPS)

### 查询

```text
7E A1 03 07 AB
```
预期返回 23 字节 A1 帧, 配置 PPS=0x0064(100).

### 相对移动 +20步

```text
7E A2 08 07 02 14 00 00 00 C7
```
成功: `7E A2 04 00 07 AD`

### 绝对移动（需先 SET_CURRENT_POSITION）

SET 当前位置为 0:
```text
7E A2 08 07 05 00 00 00 00 B6
```
成功: `7E A2 04 00 07 AD`

MOVE_ABSOLUTE 500:
```text
7E A2 08 07 01 F4 01 00 00 A7
```

### STOP/RELEASE

```text
STOP:     7E A2 04 07 03 B0
RELEASE:  7E A2 04 07 04 B1
```

### 位置无效时诊断相对移动 (≤100步)

```text
MOVE_RELATIVE +20:  7E A2 08 07 02 14 00 00 00 C7
MOVE_RELATIVE -20:  7E A2 08 07 02 EC FF FF FF 9C
```
位置有效前仍可小范围诊断运动.

### 硬件验证

- 阶段1: 不接 VM, 确认上电/复位时 STBY 保持低;
- 阶段2: 接 TB6612 不通电机, 示波器看四拍 GPIO 波形;
- 阶段3: 100 PPS 周期约 10 ms;
- 阶段4: 接风门低速小步数, 确认方向;
- 阶段5: 全行程 1850 步验证.


## 12. Step19风机健康正式停机主机测试

```sh
Tests/run_fan_health_phase2_host_test.sh
```

覆盖：PWM→预期RPM反向插值、启动加力排除、稳定等待、600/450 RPM迟滞、连续5秒转速偏差、连续5秒测速丢失、正式故障锁存、0%强制停机、锁存期间拒绝重启、清故障后继续禁止自动重启，以及显式授权后恢复启动。


## 13. Step20风机健康W2协议测试

```sh
Tests/run_fan_health_phase3_host_test.sh
```

覆盖：旧18字节查询不变、扩展V1不变、健康诊断V2 32字节布局、flags、signed偏差、计时饱和、查询失败映射、MANUAL/AUTO下清故障、严格长度、参数校验、清除失败映射，以及Master/Slave严格编译。
