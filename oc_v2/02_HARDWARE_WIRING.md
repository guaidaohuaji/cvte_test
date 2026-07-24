# 硬件连接、引脚和外设约束

## 1. 完整引脚表

| 引脚 | 外设/功能 | 软件模块 | 备注 |
|---|---|---|---|
| PA0 | ADC1_IN0 | 风机反馈 | ADC扫描rank 1 |
| PA9 | USART1_TX | W2上位机 | 9600 8N1 |
| PA10 | USART1_RX | W2上位机 | 9600 8N1 |
| PA13 | SWDIO | 调试 | 必须保留 |
| PA14 | SWCLK | 调试 | 必须保留 |
| PB1 | ADC1_IN9 | NTC | ADC扫描rank 2 |
| PB6 | TIM4_CH1 | 通用PWM输出 | AF2 |
| PB8 | TIM10_CH1 | 风机PWM | AF3 |
| PC6 | USART6_TX | 单总线发送 | AF8，38400 |
| PC7 | USART6_RX | 单总线接收 | AF8，38400 |
| PD13 | GPIO输出 | W2对象0x04 | 上电默认高 |
| PE8 | GPIO输出 | LED | 低有效 |
| PE9 | TIM1_CH1 | PWM输入 | AF1，双边沿捕获 |
| PB3 | GPIO输出 | TB6612 AIN1 | 风门步进 |
| PB4 | GPIO输出 | TB6612 AIN2 | 风门步进 |
| PB5 | GPIO输出 | TB6612 BIN1 | 风门步进 |
| PC14 | GPIO输出 | TB6612 BIN2 | 风门步进 |
| PC15 | GPIO输出 | TB6612 STBY | 风门使能 |

## 2. ADC共享链路

```text
TIM2 update TRGO，10 kHz
→ ADC1扫描两个通道
   rank1: PA0 / ADC1_IN0 / 风机反馈
   rank2: PB1 / ADC1_IN9 / NTC
→ DMA2 Stream0 circular
→ uint16_t dma_buf[1024]
```

DMA中数据交错排列：

```text
PA0, PB1, PA0, PB1, ...
```

## 3. 定时器资源

| 定时器 | 用途 | 关键配置 |
|---|---|---|
| TIM1 | PWM输入捕获 | 168 MHz / (167+1) = 1 MHz；ARR 65535；双边沿 |
| TIM2 | ADC触发 | APB1 timer clock 84 MHz；ARR 8399；10 kHz TRGO |
| TIM4 | 通用PWM输出 | PB6；运行时动态PSC/ARR |
| TIM10 | 风机PWM | PB8；PSC 167；ARR 99；10 kHz |
| TIM6 | 风门步进节拍 | 基本定时器；PSC 279；ARR 999；300 Hz；NVIC 14 |

中断优先级：

```text
USART1              10
USART6              11
TIM1_CC             12
TIM1_UP_TIM10       12
DMA2_Stream0        13
TIM6_DAC            14
SysTick             15
```

## 4. 两板单总线连接

- A板：Master
- B板：Slave
- CN18 COM相连
- SGND相连
- CN26按现有硬件选择5 V
- USART6 MCU端：PC6/PC7
- COM曾实测空闲约5 V；逻辑分析仪必须能承受对应电压

## 5. 输出极性

### PE8 LED

```text
GPIO低 -> LED亮
GPIO高 -> LED灭
```

### PD13

代码实际控制和查询的是 PD13 的 ODR电平。

`app_uart.c`中有一段注释误写为“PD15 drives...”，但实现函数和GPIO配置均为 PD13。OpenCode不得根据该错误注释改成PD15。

### PWM

- TIM4和TIM10均使用PWM1、高有效；
- 外部驱动电路是否反相必须由原理图和实测确认；
- 软件状态中的占空比是定时器侧逻辑，不自动代表最终负载端极性。

## 6. 硬件保护原则

任何后续修改不得占用或重新配置：

- PA0、PB1共享ADC；
- PB6通用PWM；
- PB8风机PWM；
- PE9 PWM输入；
- PA9/PA10 USART1；
- PC6/PC7 USART6；
- PA13/PA14 SWD；
- PE8 LED；
- PD13数字输出。

## 7. TB6612 风门驱动

### 电源

TB6612 VM   → 12 V
TB6612 VCC  → 3.3 V
TB6612 PWMA → 3.3 V（固定）
TB6612 PWMB → 3.3 V（固定）

TB6612 GND、PGND、STM32 GND、12V电源负极必须共地。

### 电机绕组

AO1 → 6号蓝线（A+）
AO2 → 1号白线（A-）
BO1 → 5号红线（B+）
BO2 → 2号黄线（B-）

3号、4号黑线为独立加热器，本轮不接入。

### 特性

- 不使用三极管、反相器、电平转换器或外部逻辑缓冲；
- STM32 3.3V GPIO 直接驱动 TB6612 CMOS 逻辑输入；
- TB6612 AIN/BIN/STBY 内部约 200 kΩ 下拉；
- 建议 PC15/STBY 增加外部 10 kΩ 下拉到 GND（待硬件确认）；
- PC14/PC15 已确认未焊接 LSE 32.768 kHz 晶振；
- PB3 使用后失去 SWO/ITM trace；
- PB4 使用后失去 NJTRST/JTAG 复位；
- PA13/PA14 SWD 仍正常可用；
- 12V 电源建议 ≥200mA。

### 风门参数（FBZA-1750-6）

- 两相双极步进，电机步距角 7.5°；
- 标称叶片步距 0.05°/step，全行程 1850 步（1850 vs 1800 待校准）；
- 每相绕组 415Ω±10%，额定 <60mA；
- 加热器 12V/1W/144Ω，独立于绕组；
- 绝缘 Class E，温升 ≤65K。
