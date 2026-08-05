# Step26 工程架构与关键数据流

## 1. 分层

```text
硬件/CubeMX层
GPIO、TIM、ADC、DMA、USART、HAL回调
        ↓
资源拥有层
app_pwm、app_pwm_input、app_adc_scan、app_onewire_uart、app_damper
        ↓
测量/执行层
app_ntc、app_fan_feedback_bpf、app_fan_feedback_adc、app_fan
        ↓
控制/安全层
app_manual_fan_control、app_auto_control、app_fan_health
        ↓
协议门面层
app_uart（W2）、app_onewire（角色门面）
```

原则：一个硬件资源只应有一个明确拥有者。上层通过 API 和 Snapshot 交换数据，不直接改下层私有全局变量。

## 2. 主循环当前真实顺序

```text
1  AppOneWireUart_Process
2  AppOneWire_Process
3  AppUart_Process
4  AppLed_Process
5  AppPwmInput_Process
6  AppNtc_Process
7  AppFan_Process
8  AppAdcScan_Process
9  AppFanFeedback_Process
10 AppDamper_Process
11 AppAutoControl_Process
12 AppManualFanControl_Process
13 AppFanHealth_Process
```

含义：W2 命令在控制器之前生效；ADC 块在本轮整理后由反馈模块消费；健康诊断最后看到本轮最终 PWM/RPM。

## 3. W2 到 PWM 输出

```text
USART1 RX IRQ
→ app_uart ring
→ handle_byte
→ A2 object 0x01
→ AppPwm_Configure
→ PSC/ARR/CCR计算
→ TIM4/PB6
```

## 4. ADC、NTC 与风机反馈

```text
TIM2 TRGO
→ ADC1 Rank1 PA0 + Rank2 PB1
→ DMA循环缓冲
→ AppAdcScan_Process
   ├─ CH0块 → Legacy/BPF测速 → AppFan
   └─ NTC平均 → AppNtc
```

## 5. 风机控制权

```text
MANUAL模式：AppManualFanControl → AppFan
AUTO模式：  AppAutoControl      → AppFan
任何模式：  AppFanHealth        → AppFan安全锁（最高优先级）
```

健康模块不直接写 TIM10，而是调用 AppFan 安全 API，保持底层状态一致。

## 6. 风门

```text
W2/AUTO命令
→ AppDamper 提交目标
→ TIM6 300 PPS中断逐步换相
→ 主循环确认完成
→ 100 ms保持
→ 释放STBY
```

上电先无条件向打开方向走 1700 步，完成后才建立 `current_steps=0` 的可信参考。

## 7. 单总线多从机

```text
W2 object 0x08, target=0x03, READ
→ AppOneWire 门面
→ Master查找/创建0x03 context
→ 链路新鲜？
   ├─ 是：直接READ
   └─ 否：HS1→HS2→READ
→ USART6共享总线
→ 只有目标地址从机应答
```

Master 不轮询、不保活。每个从机的 500 ms 超时独立，其他地址流量不刷新其计时。
