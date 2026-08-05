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
frequency: 1~840000 Hz
duty_x100 protocol field: 0~10000
duty output resolution: 1% (100 x100 units)
```

运行时选择16位PSC和ARR，并保证周期计数不少于100，从而提供1%输出档位。输入占空比按最接近的整数百分比量化，然后计算：

```text
actual_frequency
frequency_error_ppm
quantized_target_duty
actual_duty
PSC/ARR/CCR
```

对象0x01使用`AppPwm_Configure()`一次性写入频率、占空比和启用状态；0%/100%分别使用强制低/强制高模式。

### 禁用行为

`AppPwm_Configure(false, 0, 0)`：

- TIM4_CH1切换为强制低电平；
- `pwm_enabled=false`；
- 保留target frequency和target duty；
- 对象0x01查询在禁用时返回target duty。

## 2. PWM输入：对象0x03

### 硬件

```text
PE9
TIM1_CH1
PWM Input reset mode
DMA2 Stream6 Channel0
IC filter = 2
FAST/MEDIUM/SLOW counter = 21 MHz / 1 MHz / 50 kHz
ARR = 65535
DMA buffer = 128 period/high pairs
```

CH1下降沿作为周期边界和复位触发，CH2间接上升沿捕获现有语义下的external-high持续时间。主循环读取DMA最新样本并取最近5周期中位数。

Step23保持用户确认的占空比计算与极性语义不变，并保留Step22中断引擎作为回退。

### 标称范围

```text
1 Hz ~ 200000 Hz
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
R-T source = LTR LNTD5.06(05)GW, Rcent column
NTC measurement range = -40.00~120.00°C
AUTO control range = -25.00~60.00°C (independent, unchanged)
process interval = 250 ms
```

### 电阻公式

```text
Rntc = Rbottom * ADC_FS / ADC - (Rtop + Rmid + Rbottom)
```

### 温度换算

固件保存厂家表中 `-40~120°C`、每 1°C 一个 `Rcent` 阻值点。运行时按阻值单调递减特性二分查找相邻点，并使用 64 位整数线性插值输出 `0.01°C`。

```text
R(-40°C) = 64069 ohm
R(5°C)   =  5060 ohm
R(25°C)  =  1997 ohm
R(120°C) =    84 ohm
```

表外阻值不做外推：高阻端钳位到 `-40.00°C`，低阻端钳位到 `120.00°C`，并分别返回 `CLAMPED_LOW/HIGH`。自动控制仍在自身模块中把温度限制到 `-25~60°C`，因此扩大测量范围不会扩大风机或风门的自动调节区间。

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

### 手动控制模式

对象`0x06`的4字节A2 DATA为：

```text
06 | mode | value LE16
```

```text
mode=0：关闭，value必须为0
mode=1：按占空比，value=duty_x100
mode=2：按目标转速，value=1000～2300 RPM
```

`mode=0/1`兼容旧版`enable=0/1`命令。全局AUTO时三种手动命令均返回MODE_LOCKED。

### 手动目标转速控制

```text
目标RPM
→ 标定表分段线性插值得到前馈PWM
→ 风机未运行时执行100%启动加力
→ 前馈PWM
→ 等待有效测速
→ 1%/2%/3%分级闭环修正
→ ±50 RPM进入容差，超过±80 RPM退出容差
```

正常闭环PWM限制在10%～80%；100%只用于启动加力和持续无测速保护。

### 启动

新的非零启动：

```text
输出100%
→ reset反馈测量
→ STARTUP_BOOST
→ 5秒后切换目标占空比
```

### 状态

底层风机状态：

```text
OFF
STARTUP_BOOST
RUNNING
NO_TACH
TACH_UNRELIABLE
PWM_ERROR
CONFIG_ERROR
```

手动转速控制状态通过对象`0x06`扩展查询V1读取：

```text
INACTIVE
STARTING
WAIT_TACH
ADJUSTING
IN_TOLERANCE
TACH_FAULT
SATURATED_LOW
SATURATED_HIGH
HW_ERROR
```

### 查询兼容性

```text
A1 DATA 06     → 原18字节风机快照
A1 DATA 06 01  → 32字节扩展V1快照
A1 DATA 06 02  → 32字节健康诊断V2快照
```

扩展快照包含手动控制模式、目标RPM、误差、控制阶段、容差、前馈PWM、调整次数和故障次数。

### Step19 Phase 2风机健康正式停机保护

内部`AppFanHealth`根据实际PWM反向查表估算预期RPM：

```text
10%→1000, 15%→1200, 20%→1400, 30%→1700
40%→1900, 50%→2100, 60%→2200, 80%→2300
```

规则：

```text
|实际RPM-预期RPM| > 600 RPM → 进入可疑
偏差 <= 450 RPM               → 清除可疑
可疑持续5秒                   → 锁存转速偏差故障
测速无效持续5秒               → 锁存测速丢失故障
```

正式故障后：

```text
AppFan_TripSafetyFault()
→ 立即输出0%
→ 底层拒绝所有enable=true请求
→ AUTO风机控制状态进入SAFETY_LOCKED
→ 手动风机控制清除历史命令并进入SAFETY_LOCKED
```

清除故障后仍保持0%，并继续禁止自动重启；只有明确的新手动命令或重新发送AUTO模式命令才会调用重启授权。Step20已把清故障命令和详细诊断字段加入W2对象0x06：`mode=3,value=0`清除锁存，`A1 DATA 06 02`读取健康诊断V2。

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

- Step24上电: 自动沿打开方向强制运行1700步，状态BOOT_HOMING，position_valid=false
- 完成后建立0步=全开、90°参考，保持100ms后STBY=0释放
- 校准期间绝对/相对/SET_CURRENT_POSITION返回BUSY；中止后位置仍无效
- SET_CURRENT_POSITION仍可在校准被中止后人工建立参考
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


## 10. Step25单总线多从机按需事务

Master使用一个共享物理事务状态机和最多8个从机上下文。上电不发单总线数据；
只有W2对象0x08提交读写或显式握手时才访问总线。每个从机上下文独立判断450 ms握手有效期，
失效时自动完成两次握手并继续原读写。Slave地址由`APP_ONEWIRE_SLAVE_ADDRESS`确定，
只处理发给本机的帧；其他节点流量静默忽略，不刷新本机500 ms通信故障计时。
