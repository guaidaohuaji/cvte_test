# V2源码提取说明

## 1. 依据

源码副本：

```text
cvte_test-step11-led-arbitration.zip
SHA256: 65e30ab405b10481fa6030d2bebcdcac924979193547c477b8a4b96d64c675ae
```

提取为只读分析，没有修改工程源码。

## 2. 重点源码事实

- `main.c`初始化了TIM1、TIM2、TIM4、TIM10、ADC1、DMA和两路UART；
- `app_uart.c`实际支持对象`0x01、0x02、0x03、0x04、0x05、0x06、0x08`；
- PA0与PB1共用ADC1扫描；
- TIM2以10 kHz触发ADC；
- 风机反馈不是定时器捕获，而是ADC波形分析；
- 通用PWM输出和风机PWM是两套独立定时器；
- PWM输入当前默认使用TIM1 PWM Input + DMA自动量程；Step22双边沿捕获仅作为回退；
- 既有模块没有独立host测试。

## 3. 源码与旧交接资料的差异

V1没有完整说明：

- PB8/TIM10风机PWM；
- PA0 ADC风机反馈；
- PB1 NTC；
- TIM2/ADC/DMA数据链；
- PB6/TIM4通用PWM；
- PE9/TIM1 PWM输入；
- 对象0x01~0x06的字段和命令；
- 既有功能静态风险。

V2已补齐。

## 4. 源码内部不一致

### PD13/PD15

`app_uart.c`注释写PD15，但：

- 函数名`pd13_get/pd13_set`；
- GPIO访问`GPIOD GPIO_PIN_13`；
- `gpio.c`配置PD13；
- W2对象名OBJ_PD13。

应以PD13为准。

### NTC采样宏

`APP_NTC_SAMPLE_COUNT`等配置与实际累积窗口不一致，详见风险文件。

### PWM输入高电平时间

用户已确认现有对象0x03占空比语义正确。Step23通过CH1下降沿周期边界和CH2间接上升沿捕获保持同一external-high定义，不再把该语义列为待修正问题。

## 5. 证据等级

本文件中的数值、函数和数据布局来自源码。

下列属于推断：

- 风机RPM系数30隐含2脉冲/转；
- PWM输入DMA自动量程在目标板上的切换边界和窄脉冲能力；
- DMA活缓冲存在一致性风险。

推断已明确标注，不能作为已发生故障。
