# 已知风险、限制和待确认项

以下均来自第11步源码静态提取。除非明确写“已发生”，否则不是硬件已发生故障。

## P0：保护已验证单总线链路

不得破坏：

```text
对象0x08提交
→ 必要时重新握手
→ 执行一次事务
→ A1保留最近结果
```

## P1：Master/Slave TX异常恢复

与V1相同：

- Slave `response_in_flight`依赖TX完成事件清除；
- Master业务TX watchdog不一定终止HAL底层TX；
- 属于静态恢复缺口，当前硬件日志未显示已触发。

## 已确认：PWM输入占空比语义保持现状

用户已确认当前对象`0x03`占空比计算和显示语义正确。Step22明确不修改：

```c
high = current_rise - last_fall;
```

以及`INPUT_INVERTED`和`ext_high`处理。后续任何高频架构改造都必须通过回归测试保持该语义。

## P1：PWM输入Step23 DMA自动量程待实物验证

Step23已将默认引擎改为TIM1 PWM Input + DMA循环采集，消除每边沿HAL中断负载，并把软件范围扩展到1～200 kHz。仍需验证：

- DMA2 Stream6 Channel0与TIM1 DMA burst在目标板上的连续运行；
- 200 kHz、1%约一个计数时的电气可识别性；
- FAST/MEDIUM/SLOW量程切换时的短暂SEARCH窗口；
- USART1阻塞、单总线、风机和风门同时运行时DMA缓冲是否稳定；
- `dma_error_count`和`profile_switch_count`长期是否异常增长。

若实测异常，可将`APP_PWM_INPUT_ENGINE_MODE`一行回退到Step22中断引擎。

## P1：共享ADC DMA缓冲一致性

`AppAdcScan_GetCh0Block()`返回正在被DMA循环写入的原始缓冲指针。

风机反馈在full complete后遍历全部1024 halfword，此时DMA已经回绕并可能开始覆盖前半区。存在实时一致性风险。

同时，half callback不递增`dma_seq`，`AppAdcScan_Process()`通常直到full callback后才处理half数据。

当前未保存实际overrun或错误RPM证据。

## P1：NTC配置与实际采样行为不完全一致

以下宏在当前实现中没有真正控制采样：

```text
APP_NTC_SAMPLE_COUNT = 32
APP_NTC_ADC_TIMEOUT_MS = 2
APP_NTC_RATIOMETRIC = 1
APP_NTC_SUPPLY_MV
```

实际行为是每250 ms取出自上次读取以来累积的所有rank2样本平均值，通常远多于32个。

修改前应先确认产品需求到底是32点平均还是时间窗平均。

## P2：PWM输入状态枚举未完全使用

枚举定义了：

```text
OUT_OF_RANGE
UNSTABLE
HW_ERROR
```

但当前处理代码主要只写：

```text
SEARCH
OK
STATIC_HIGH
STATIC_LOW
```

无效周期通常会退回未同步状态，而不是明确报告OUT_OF_RANGE。

## P2：风机低于100%时固定标为TACH_UNRELIABLE

任何目标/实际占空比低于100%时，状态设置为：

```text
APP_FAN_STATE_TACH_UNRELIABLE
```

即使反馈频率有效，仍保留该状态并继续更新RPM。

需要确认这是硬件反馈在PWM调速下不可靠的产品定义，还是临时诊断策略。

## P2：风机RPM系数隐含2脉冲/转

```c
rpm = feedback_frequency_hz * 30
```

这等价于假设2 pulses/revolution。源码没有单独的PPR宏或硬件说明。

更换风机时必须确认PPR。

## P2：初始化错误处理不一致

`main.c`：

- PWM、PWM输入失败会进入Error_Handler；
- NTC、Fan、ADC scan失败返回值被忽略。

这可能导致系统继续运行，但对象状态进入错误或采样永远无数据。

## P2：W2旧对象接受额外尾部数据

对象0x01、0x02、0x04、0x06控制使用最小长度检查，不是严格长度。直接收紧可能影响既有上位机兼容性。

## P2：PD13注释写成PD15

实际GPIO、函数和对象均为PD13。只修注释风险低，但不要据此改变硬件引脚。

## P2：通用PWM禁用快照语义

`AppPwm_Enable(false)`将CCR设为0，但：

- `pwm_running`仍置true；
- 查询对象0x01在disabled时返回目标占空比，不返回实际0；
- actual frequency仍是此前频率。

这是当前接口语义，不要未经版本说明改变。

## P2：既有模块缺少专用单元测试

PWM、PWM输入、ADC、Fan、风机反馈仍需持续完善独立host测试；Step27已为NTC Rcent表、端点和插值补充专用host测试。

## P3：NTC模型和硬件拓扑需核对

当前电阻公式：

```text
Rntc = Rbottom * 4095 / ADC - (Rtop + Rmid + Rbottom)
```

配置：

```text
Rtop=5240
Rmid=10000
Rbottom=10000
R-T表 = LTR LNTD5.06(05)GW Rcent，-40~120°C
```

分压电阻值和拓扑仍必须与实板一致。用户已确认本次硬件电路未改变；仍建议用电阻箱或温箱验证端点和若干中间点。

## V3 新增: 风门驱动风险

### ✅ 已校准：全行程 (V3)

实物实测约 1700 步全开→全关。`APP_DAMPER_FULL_TRAVEL_STEPS` 
已从标称 1850 更新为实测 1700。

### ✅ 已确认：正向方向 (V3)

正向 (0→1→2→3) 实物确认为关闭。逻辑坐标: 0步=全开, 
1850步=全关。current_steps递增=关闭, MOVE_RELATIVE正数=关闭。
`APP_DAMPER_FORWARD_IS_OPEN=0` 已反映实物结果, 该宏仅作说明不参与方向计算。

### P3：HardFault 等致命异常无风门关闭

HardFault/MemManage/BusFault/UsageFault/NMI 已接入 EmergencyShutdown (best-effort)，但若异常源于栈损坏/总线不可访问/代码取指故障，关闭函数可能无法完整执行。当前无硬件级安全关断（如独立看门狗触发 TB6612 STBY 外部电路）。

### P3：STBY 外部下拉未确认

方案建议 PC15/STBY 增加外部 10 kΩ 下拉到 GND。若未安装，需在硬件测试阶段确认 MCU 上电/复位/下载期间 STBY 保持低。

### P2：CubeMX 覆盖风险

TIM6 初始化和 MSP 在 CubeMX 保护区外（`tim.c`），重新生成会被覆盖。风险等级：中等。

### P2：风门模块无独立 host 单元测试

`Tests/` 目录中无 `app_damper.c` 的独立测试。

## 推荐后续顺序

1. 归档黄金固件和全部W2对象查询。
2. 用标准信号源确认PWM输入占空比方向。
3. 用示波器验证TIM4/TIM10。
4. 用电阻箱/温箱验证NTC。
5. 用转速仪确认风机PPR和RPM。
6. 做ADC overrun和长时压力测试。
7. 风门硬件验证 (100 PPS 空载起步 → 方向确认 → 1850步校准 → 300 PPS).
8. USART1 28ms 阻塞与 USART6 3ms 换向保护并发测试。
7. 再决定是否修复上述静态风险。


## Step20 Phase 3风机健康诊断待办

- 阈值600 RPM、恢复阈值450 RPM、确认5秒仍需实机标定；
- 必须覆盖10%、15%、20%、30%、40%、50%、60%、80%稳定工况，确认无误报；
- 必须验证启动boost结束、AUTO大目标跳变、手动PWM变化、风阻突变和供电波动；
- W2清故障命令已加入，仍需实机验证清除后保持0%且不会自动重启；
- 对象0x06已新增独立健康诊断V2，需验证旧上位机收到`06 02`时的18字节兼容回退；
- 上位机应区分“转速偏低、转速偏高、测速丢失、停机锁存”，并在锁存时禁用启动按钮；
- ARM交叉编译和真实目标板验证尚未完成。


## Step21 通用PWM输出剩余验证

- 用户已确认100 kHz/1%输出问题修复；
- 840 kHz/1%仅有一个定时器计数，仍属于软件可配置上限，不等于全部高频电气性能已验证；
- 上位机若仍限制100 kHz，需要后续单独更新其输入范围。

## Step23 通用PWM输入剩余验证

- 正式软件范围为1～200 kHz，但尚未保存标准信号源或PB6回环的目标板DMA测试证据；
- 必须验证1%、50%、99%等占空比以及静态高/低；
- 必须观察200 kHz连续运行时`dma_error_count`、USART1、单总线、风机和风门是否稳定；
- 必须跨100 Hz和5 kHz附近验证自动量程切换；
- 200 kHz、1%仅约一个计数，必须以示波器和对象0x03实测为准。
