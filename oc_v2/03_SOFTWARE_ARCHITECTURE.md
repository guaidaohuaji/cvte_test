# 软件架构、初始化顺序与数据流

## 1. `main.c`初始化顺序

真实源码顺序：

```c
MX_GPIO_Init();
MX_DMA_Init();
MX_USART1_UART_Init();
MX_USART6_UART_Init();
MX_TIM4_Init();
MX_TIM1_Init();
MX_ADC1_Init();
MX_TIM10_Init();
MX_TIM2_Init();
#if APP_DAMPER_ENABLED
MX_TIM6_Init();       # Master 才执行
#endif

AppUart_Init();
AppOneWireUart_Init();
AppOneWire_Init();
AppLed_Init();
AppPwm_Init();
AppPwmInput_Init();
AppNtc_Init();
AppFan_Init();
AppAdcScan_Init();
AppDamper_Init();     # 角色内部隔离
```

其中：

- `AppPwm_Init()`失败会进入`Error_Handler()`；
- `AppPwmInput_Init()`失败会进入`Error_Handler()`；
- `AppNtc_Init()`、`AppFan_Init()`、`AppAdcScan_Init()`返回值被忽略；
- 后三者的失败需要通过状态查询或调试变量判断。

## 2. 主循环顺序

```c
AppUart_Process();
AppOneWireUart_Process();
AppOneWire_Process();
AppLed_Process();
AppPwmInput_Process();
AppNtc_Process();
AppFan_Process();
AppAdcScan_Process();
AppFanFeedback_Process();
AppDamper_Process();           # POST_MOVE_HOLD 100ms 释放
```

影响：

- W2控制先于业务状态机；
- LED使用本轮最新单总线状态；
- NTC和Fan在本轮读取的是此前已整理的数据；
- ADC扫描整理和风机反馈处理位于循环尾部，通常造成一个主循环级延迟；
- 所有业务都必须保持非阻塞。

## 3. 中断与回调

### UART

`main.c`集中定义：

```text
HAL_UART_RxCpltCallback
HAL_UART_TxCpltCallback
HAL_UART_ErrorCallback
```

### PWM输入

`main.c`：

```text
HAL_TIM_IC_CaptureCallback -> AppPwmInput_CC_Callback
HAL_TIM_PeriodElapsedCallback -> AppPwmInput_UP_Callback (TIM1)
                              -> AppDamper_TimerCallback   (TIM6)
```

### TIM6 步进节拍

`stm32f4xx_it.c` 中 `TIM6_DAC_IRQHandler` → `HAL_TIM_IRQHandler(&htim6)` → `HAL_TIM_PeriodElapsedCallback` 分发给 TIM6。

TIM6_DAC_IRQHandler 是 TIM6 和 DAC 的共享中断入口。当前不初始化 DAC。

### ADC DMA

`app_adc_scan.c`直接定义：

```text
HAL_ADC_ConvHalfCpltCallback
HAL_ADC_ConvCpltCallback
```

后续不得在其他文件重复定义这些HAL回调。

## 4. 应用数据流

### 通用PWM输出

```text
USART1对象0x01
→ AppPwm_SetFrequency
→ AppPwm_SetDutyX100
→ AppPwm_Enable
→ TIM4_CH1 / PB6
```

### PWM输入

```text
PE9 / TIM1_CH1双边沿
→ TIM1_CC和UPDATE中断
→ AppPwmInput原始捕获
→ AppPwmInput_Process
→ 对象0x03快照
```

### 风机控制

```text
USART1对象0x06
→ AppManualFanControl
   ├─ OFF：关闭
   ├─ DUTY：手动占空比
   └─ SPEED：前馈查表 + 闭环目标转速
→ AppFan底层启动boost/测速状态机
→ TIM10_CH1 / PB8
```

全局AUTO仍由`AppAutoControl`独占风机和风门；手动控制模块检测到AUTO后清除历史命令，不会与AUTO同时写PWM。

### 风机反馈

```text
PA0 ADC1_IN0
→ ADC1 DMA交错缓冲
→ AppAdcScan_GetCh0Block
→ 移动平均+双阈值+边沿确认
→ 频率
→ AppFan换算RPM
→ 对象0x06快照
```

### NTC

```text
PB1 ADC1_IN9
→ ADC1 DMA交错缓冲
→ AppAdcScan_GetNtcAverage
→ 电压/电阻/B参数温度
→ 对象0x05快照
```

## 5. 模块职责

### `app_adc_scan.c`

- 启动ADC1 DMA；
- 启动TIM2；
- 管理1024 halfword循环缓冲；
- 累加rank2 NTC样本；
- 向风机反馈提供rank1缓冲；
- 记录overrun。

### `app_pwm.c`

- TIM4动态频率参数计算；
- 频率1～100000 Hz；
- 占空比0～10000；
- 返回实际频率、误差ppm、PSC/ARR/CCR。

### `app_pwm_input.c`

- TIM1 1 MHz扩展时间戳；
- 双边沿捕获；
- 过捕获恢复；
- 静态电平和超时识别；
- 返回频率mHz、占空比x100和age。

### `app_fan.c`

- TIM10风机PWM；
- 启动boost；
- 目标/实际占空比；
- 风机状态；
- 无测速超时；
- RPM快照。

### `app_fan_feedback_adc.c`

- 对PA0 ADC数据做10点移动平均；
- 高低双阈值；
- 最小持续样本确认；
- 周期范围过滤；
- 连续3个好周期后发布频率。

### `app_ntc.c`

- 读取共享ADC平均；
- 计算ADC电压；
- 按当前分压模型计算电阻；
- B参数计算温度；
- 0～60°C窗口分类。

### `app_uart.c`

- USART1 128字节ring；
- W2解析；
- 对象`0x01~0x08`；
- 50 ms帧超时；
- 阻塞式短帧回复`HAL_UART_Transmit(...,100ms)`。

### `app_damper.c` (V3 新增)

- TB6612 风门两相四拍步进电机控制；
- TIM6 100 Hz 固定节拍换相；
- 预计算 BSRR 相位表跨 GPIOB/GPIOC 原子更新；
- 位置无效诊断运动（≤100 步）；
- 到位 100 ms 保持后自动释放；
- EmergencyShutdown 接入 Error_Handler 和致命异常处理器；
- Master/Slave 角色编译期隔离（APP_DAMPER_ENABLED）。

### `app_damper_config.h`

- APP_DAMPER_FULL_TRAVEL_STEPS (1850)
- APP_DAMPER_STEP_PPS (100)
- APP_DAMPER_DIAG_MAX_RELATIVE_STEPS (100)
- APP_DAMPER_POST_MOVE_HOLD_MS (100)
- APP_DAMPER_FORWARD_IS_OPEN (1, 待实物确认)
- TIM6 PSC=279, ARR=2999

## 6. 架构保护原则

- 不在中断里做浮点温度计算或完整协议状态机；
- 不把ADC1拆成两个互相竞争的启动流程；
- 不让风机反馈和NTC分别调用`HAL_ADC_Start*`；
- 不重复定义ADC/TIM/UART回调；
- 不使用`HAL_Delay()`处理通信或采样；
- 修改共享ADC时必须同时评估风机反馈和NTC。
