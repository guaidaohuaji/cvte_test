# 项目环境、功能范围与产品行为

## 1. 平台

- MCU：STM32F407VET6
- Flash：512 KB
- 框架：STM32CubeMX 生成工程 + STM32 HAL
- 架构：裸机主循环、非阻塞业务模块
- 系统时钟：168 MHz
- 调试下载：J-Link / SWD
- 当前可靠构建入口：`build.ps1`
- 工具链：`arm-none-eabi-gcc`
- SWD：PA13、PA14 必须保留

## 2. 当前应用功能

| 功能 | 模块 | 外设/引脚 |
|---|---|---|
| USART1 W2 上位机协议 | `app_uart.c` | PA9/PA10，9600 8N1 |
| 板间单总线 | `app_onewire_*` | USART6 PC6/PC7，38400 8N1 |
| 通用 PWM 输出 | `app_pwm.c` | TIM4_CH1 / PB6 |
| 通用 PWM 输入检测 | `app_pwm_input.c` | TIM1_CH1 / PE9 |
| 风机 PWM 控制 | `app_fan.c` | TIM10_CH1 / PB8 |
| 风机反馈检测 | `app_fan_feedback_adc.c` | ADC1_IN0 / PA0 |
| NTC 检测 | `app_ntc.c` | ADC1_IN9 / PB1 |
| ADC 共享扫描 | `app_adc_scan.c` | ADC1 + TIM2 TRGO + DMA2 Stream0 |
| LED | `app_led.c` | PE8，低有效 |
| 数字输出 | `app_uart.c` | PD13 |

## 3. USART1/W2 对象

```text
0x01 通用 PWM 输出
0x02 PE8 LED
0x03 PWM 输入检测
0x04 PD13 数字输出
0x05 NTC 查询
0x06 风机控制与状态
0x08 单总线控制与状态
```

`0x01~0x06`是原工程既有接口，后续单总线或其他开发不得破坏。

## 4. 单总线主从角色

```powershell
.\build.ps1 -Role master
.\build.ps1 -Role slave
```

- Master：`APP_ONEWIRE_ROLE=1`
- Slave：`APP_ONEWIRE_ROLE=2`

输出：

```text
build_master\test_master.elf/.hex/.bin/.map
build_slave_02\test_slave_02.elf/.hex/.bin/.map
build_slave_03\test_slave_03.elf/.hex/.bin/.map
```

## 5. 当前冻结的单总线行为

### Master

```text
上电保持总线空闲，不自动握手、不扫描、不轮询保活
→ 收到指定从机的读写命令后检查该从机握手有效性
→ 无效时先握手，再自动继续原读写
```

对象 `0x08` 支持默认从机0x02旧格式和显式目标地址新格式。收到握手、写、读命令后：

```text
链路有效
→ 执行一次事务

链路无效或 STALE
→ 自动重新握手
→ 执行 pending 事务一次
```

### Slave

- 上电等待握手；
- 在线后接受读写；
- 500 ms 无正确请求进入通信故障；
- 任意状态允许正确握手1重新建立会话；
- 寄存器 `0x0000~0x0100`位于 RAM，上电清零。

## 6. 既有功能当前行为概览

### 通用 PWM 输出

- 默认启动：1 kHz、50%、已启用；
- 频率范围：1 Hz～840 kHz；
- 输出占空比按1%量化，0%和100%使用强制电平模式；
- 占空比单位：`x100`，`10000 = 100.00%`；
- 关闭时输出 CCR=0，但保留目标频率和目标占空比。

### PWM 输入

- 输入：PE9 / TIM1_CH1；
- TIM1硬件PWM Input + DMA循环采集；
- FAST/MEDIUM/SLOW自动量程：21 MHz、1 MHz、50 kHz；
- 输入数字滤波器：2；
- 标称软件范围：1～200000 Hz；
- 最近5周期中位数；
- 2.5 s无有效周期后报告静态高或静态低。

### 风机

- PWM：PB8 / TIM10_CH1；
- 固定 10 kHz；
- 非零首次启动先 100% boost 2 s；
- 合法非零目标占空比：10.00%～100.00%；
- 风机反馈由 PA0 ADC 波形检测；
- 500 ms 无新测速可进入 `NO_TACH`。

### NTC

- 输入：PB1 / ADC1_IN9；
- 每 250 ms消费一次共享 ADC 平均值；
- 使用厂家 `LNTD5.06(05)GW` R-T 表的 `Rcent` 列查表并在相邻 1°C 点间插值；
- NTC测量有效范围：-40.00～120.00 °C；
- AUTO控制温度范围独立保持-25.00～60.00 °C；
- 对象 `0x05`只读。

## 7. 验证边界

已保存硬件证据：

- 单总线写入、自动重新握手和读回。

没有在本交接中保存明确硬件证据：

- 通用 PWM 输出精度；
- PWM 输入频率和占空比；
- 风机真实启停和 RPM；
- NTC 温度精度；
- PD13 外部负载逻辑；
- PE8所有自动闪烁周期。
