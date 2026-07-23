# 既有应用功能源码提取

## 1. 通用PWM输出：对象0x01

### 硬件

```text
TIM4_CH1
PB6
PWM1，高有效
timer clock = 84 MHz
```

### 默认状态

`AppPwm_Init()`调用：

```text
frequency = 1000 Hz
duty = 5000 = 50.00%
enabled = true
```

### 范围和参数计算

```text
frequency: 1~100000 Hz
duty_x100: 0~10000
```

运行时选择16位PSC和ARR，计算：

```text
actual_frequency
frequency_error_ppm
actual_duty
PSC/ARR/CCR
```

### 禁用行为

`AppPwm_Enable(false)`：

- CCR设为0；
- `pwm_enabled=false`；
- 保留target frequency和target duty；
- 对象0x01查询在禁用时返回target duty。

## 2. PWM输入：对象0x03

### 硬件

```text
PE9
TIM1_CH1
both-edge input capture
IC filter = 15
counter = 1 MHz
ARR = 65535
```

使用overflow count扩展为64位时间戳。

### 标称范围

```text
1 Hz ~ 5000 Hz
timeout = 2500 ms
```

### 输出

```text
status
frequency_millihz
duty_x100
period_ticks
ext_high_ticks
age_ms
overcapture_count
raw pin
```

### 静态识别

超过2.5 s无有效捕获：

- 当前PE9高：STATIC_HIGH；
- 当前PE9低：STATIC_LOW。

### 重要待确认

当前上升沿处用`rise - last_fall`作为`raw_high_ticks`，这在普通非反相信号中通常是低电平时间。必须实测确认占空比是否互补。

## 3. ADC共享扫描

### 硬件

```text
TIM2 TRGO 10 kHz
ADC1 12-bit scan
rank1 PA0 / ADC1_IN0
rank2 PB1 / ADC1_IN9
DMA2 Stream0 circular
buffer = 1024 halfwords
```

每次TIM2触发完成两个转换。

### 数据消费者

- 偶数索引：PA0，风机反馈；
- 奇数索引：PB1，NTC；
- 半满/全满回调设置标志；
- full callback递增`dma_seq`。

### NTC数据

`AppAdcScan_Process()`累计奇数索引样本，`AppAdcScan_GetNtcAverage()`返回并清零累计窗口。

### 风机数据

`AppAdcScan_GetCh0Block()`返回整个DMA缓冲指针和`dma_seq`。

## 4. NTC：对象0x05

### 配置

```text
ADC full scale = 4095
ADC reference = 3300 mV
Rtop = 5240 ohm
Rmid = 10000 ohm
Rbottom = 10000 ohm
Rref = 5060 ohm
Tref = 278.150 K = 5.00°C
B = 3839 K
valid temperature = 0.00~60.00°C
process interval = 250 ms
```

### 电阻公式

当前代码：

```text
Rntc = Rbottom * ADC_FS / ADC - (Rtop + Rmid + Rbottom)
```

### 温度公式

```text
1/T = 1/Tref + ln(R/Rref)/B
```

输出单位为0.01°C。

### 状态

```text
SEARCH
OK
ADC_ERROR
OPEN_OR_UNDER_TEMP
SHORT_OR_OVER_TEMP
CALC_ERROR
CONFIG_ERROR
```

`adc==0`被归为OPEN_OR_UNDER_TEMP；无法计算且非0通常归为SHORT_OR_OVER_TEMP。

## 5. 风机PWM和状态：对象0x06

### 硬件

```text
TIM10_CH1
PB8
10 kHz
PSC=167
ARR=99
```

### 合法控制

```text
disable: enable=0, duty=0
enable + zero duty: 合法，enabled=true但输出0/state OFF
enable + nonzero duty: 1000~10000
```

### 启动

新的非零enable：

```text
输出100%
→ reset反馈测量
→ STARTUP_BOOST
→ 2秒后切换目标占空比
```

### 状态

```text
OFF
STARTUP_BOOST
RUNNING
NO_TACH
TACH_UNRELIABLE
PWM_ERROR
CONFIG_ERROR
```

100%时进入RUNNING；低于100%时进入TACH_UNRELIABLE。

### 无测速

启动2 s宽限后，如果500 ms没有新的反馈更新：

```text
NO_TACH
freq=0
rpm=0
```

新反馈可恢复到RUNNING或TACH_UNRELIABLE。

## 6. 风机反馈算法

### ADC输入

PA0 / ADC1_IN0，10 ksample/s。

### 滤波和判决

```text
10点移动平均
LOW_THRESHOLD  = 900
HIGH_THRESHOLD = 1800
高候选持续至少20样本
低候选持续至少5样本
```

### 频率范围

```text
10~120 Hz
```

需要连续3个合法周期后发布：

```text
freq_millihz
period_samples
update_seq
```

### RPM

```text
rpm = frequency_millihz * 30 / 1000
```

即：

```text
rpm = frequency_hz * 30
```

隐含2脉冲/转，需与实际风机确认。

## 7. LED：对象0x02

PE8低有效。

模式：

```text
AUTO=0
MANUAL=1
```

控制：

```text
0 手动灭
1 手动亮
2 自动
```

自动：

- Master：1000/1000 ms；
- Slave等待：灭；
- Slave ONLINE：500/500 ms；
- Slave故障：200/200 ms。

## 8. PD13：对象0x04

- 上电默认GPIO高；
- 查询返回PD13 ODR；
- 控制0=低、1=高；
- 外部电路是否反相需硬件确认；
- `app_uart.c`中“PD15”注释是笔误，代码为PD13。

## 9. 与单总线的资源关系

这些既有功能与单总线不共享USART6，但共享：

- 主循环CPU时间；
- SysTick；
- USART1 W2分发；
- HAL回调命名空间；
- NVIC优先级；
- `build.ps1`源文件列表。

因此新增功能仍可能间接影响单总线实时性。

## 10. TB6612风门驱动 (V3新增)

### 硬件

- TB6612双H桥, VM=12V, VCC=PWMA=PWMB=3.3V
- PB3→AIN1, PB4→AIN2, PB5→BIN1, PC14→BIN2, PC15→STBY
- TIM6固定100Hz步进节拍 (PSC=279, ARR=2999)
- TIM6_DAC_IRQHandler共享中断入口, 不使用DAC

### 四拍相序

四拍两相通电, 正向: 0→1→2→3→0

### 运动控制

- 上电: position_valid=false, POSITION_UNKNOWN, STBY=0
- SET_CURRENT_POSITION 建立参考后可使用绝对/相对移动
- 到位后保持最后相位100ms→自动释放 (STBY=0)
- 位置无效时可诊断相对移动 (≤100步)
- 零步命令立即成功, 不启动TIM6

### 对象0x07

A1: 23字节 (状态+位置+命令+结果)
A2: MOVE_ABSOLUTE/RELATIVE/STOP/RELEASE/SET_CUR_POS
全部严格长度检查. A2 OK 仅表示命令已接受.

### 故障安全

Error_Handler + HardFault/MemManage/BusFault/UsageFault/NMI
均接入 EmergencyShutdown (best-effort):
TIM6 stop → STBY=0 → AIN/BIN=0 → state=FAULT
