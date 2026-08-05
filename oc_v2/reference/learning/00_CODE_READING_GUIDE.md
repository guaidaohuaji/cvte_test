# Step26 代码阅读顺序与学习计划

## 0. 先明确工程的运行模型

本工程不是 RTOS，也不是“每个功能一个线程”。核心模型是：

```text
外设中断：搬运一个字节、置一个标志、记录一次捕获或推进一个步进相位
                         ↓
main.c while(1)：多个 AppXXX_Process() 依次运行并快速返回
                         ↓
Snapshot：模块把当前事实复制给 W2 查询、控制器和诊断模块
```

阅读时始终问四个问题：

1. 谁调用这个函数？中断还是主循环？
2. 这个模块拥有哪些硬件资源和全局状态？
3. 命令是立即完成，还是只提交后由状态机异步完成？
4. 对外返回的是目标值、实际值，还是一次诊断快照？

---

## 第一阶段：先搭出全局骨架（建议 1~2 小时）

### 1. `build.ps1`

先看编译角色：

```text
APP_ONEWIRE_ROLE=1 -> Master
APP_ONEWIRE_ROLE=2 -> Slave
APP_ONEWIRE_SLAVE_ADDRESS -> 从机本机地址
```

确认三种输出目录：

```text
build_master/
build_slave_02/
build_slave_03/
```

学习目标：理解同一份源码如何编译成三块板的不同固件。

### 2. `Core/Src/main.c`

只看三部分：

1. `MX_*_Init()`：CubeMX 生成的硬件初始化；
2. `App*_Init()`：应用模块初始化依赖；
3. `while(1)`：所有非阻塞任务的调度顺序。

然后看文件末尾 HAL 回调如何按外设实例转发。

### 3. 所有 `*_config.h`

按以下顺序浏览，不要先钻进算法：

```text
app_pwm_input_config.h
app_ntc_config.h
app_fan_config.h
app_damper_config.h
app_auto_control_config.h
app_fan_health_config.h
app_onewire_config.h
```

把频率、时间、门限、方向和角色宏抄到自己的笔记中。后续看到“为什么是 5000 ms”“为什么 1700 步”时，先回配置文件找答案。

---

## 第二阶段：从最简单的模块建立阅读信心（约半天）

### 4. `app_led.h/.c`

这是最适合入门的状态机：手动/自动模式、tick 计时、低有效 GPIO。

建议跟踪：

```text
AppLed_SetAutomatic()
→ determine_auto_profile()
→ profile_interval_ms()
→ apply_output()
```

### 5. `app_pwm.h/.c`

重点公式：

```text
计数时钟 = 84 MHz / (PSC + 1)
周期计数 = ARR + 1
输出频率 = 84 MHz / ((PSC + 1) × (ARR + 1))
PWM1高电平计数约等于 CCR
```

阅读路径：

```text
AppPwm_Configure()
→ quantize_duty_x100()
→ compute_frequency_params()
→ compute_ccr_percent()
→ apply_enabled_configuration()
```

重点观察：代码先在局部变量中完成计算，最后才停表写寄存器；启动失败会回滚。

### 6. `app_ntc_config.h` 与 `app_ntc.c`

阅读路径：

```text
AppNtc_Process()
→ do_sample()
→ calc_voltage_mv()
→ calc_resistance()
→ calc_temp()
→ apply_range_status()
```

先理解数据单位：ADC count、mV、Ω、0.01°C。

---

## 第三阶段：理解“中断只置标志，主循环处理数据”（约 1 天）

### 7. `app_adc_scan.h/.c`

画出 DMA 缓冲：

```text
索引 0: PA0/CH0 风机反馈
索引 1: PB1/CH9 NTC
索引 2: PA0/CH0
索引 3: PB1/CH9
...
```

阅读 HAL 半满/全满回调，再看 `AppAdcScan_Process()` 如何发布数据块。


### 7.1 `app_ntc_config.h`、`app_ntc.h` 与 `app_ntc.c`

按以下数据链阅读：

```text
AppAdcScan_GetNtcAverage()
→ calc_voltage_mv()
→ calc_resistance()
→ calc_temp()
   ├─ 检查 -40°C / 120°C 表端点
   ├─ 在 161 个 Rcent 点中二分查找
   └─ 对相邻 1°C 点做整数线性插值
→ apply_range_status()
```

注意区分两个范围：NTC模块现在可测并上报 `-40~120°C`；AUTO模块仍只在 `-25~60°C` 内线性映射，范围外在 `evaluate_targets()` 中钳位到冷热端目标。

### 8. `app_pwm_input_config.h` 与 `app_pwm_input.c`

先只看默认 DMA 分支，不要同时阅读 Legacy 分支。

阅读顺序：

```text
AppPwmInput_Init()
→ start_profile(FAST)
→ dma_hw_start_profile()
→ AppPwmInput_Process()
   ├─ 读取 DMA producer
   ├─ 检查溢出/错误
   ├─ 自动切量程
   ├─ collect_recent_pairs()
   ├─ 周期中位数
   └─ 计算频率与占空比
```

掌握每个 DMA pair：`CCR1 周期 + CCR2 外部高时间`。之后再阅读 Step22 Legacy 分支，比较“每边沿中断”和“硬件 PWM Input + DMA”的差异。

---

## 第四阶段：风机数据链（建议 1~2 天）

### 9. `app_fan_feedback_bpf.h/.c`

先不纠结滤波器系数来源，只理解数据流：

```text
ADC样本
→ 两级 biquad 带通
→ 迟滞过零
→ 周期连续性检查
→ 最近5周期中位数
→ 频率有效/超时
```

重点函数：`process_biquad()`、`process_crossing()`、`accept_period()`。

### 10. `app_fan_feedback_adc.h/.c`

这是正式结果仲裁层：

```text
Legacy测速 + BPF测速
→ bpf_result_is_usable / legacy_result_is_usable
→ 选择正式 source
→ AppFanFeedbackSnapshot
```

阅读时区分“内部诊断值”和“最终正式频率”。

### 11. `app_fan_config.h` 与 `app_fan.c`

阅读：

```text
AppFan_SetEnabled()
→ 5秒 startup boost
→ AppFan_Process()
→ 正式 FG 频率 × 30 = RPM
```

然后看安全锁：Trip、Clear、AuthorizeRestart 三者不是同一件事。

### 12. `app_auto_fan_profile.h`

手算一两个插值例子，例如 1800 RPM 位于 1700/1900 RPM 两点之间，验证输出占空比。

### 13. `app_manual_fan_control.c`

跟踪目标转速命令：前馈占空比、等待测速、误差连续确认、1/2/3%调节、容差迟滞。

### 14. `app_fan_health_config.h` 与 `app_fan_health.c`

按四阶段画状态图：

```text
SETTLING
→ MONITORING
→ SUSPECT（偏差或无测速）
→ 连续5秒
→ LATCHED + 强制停机
```

重点区分：

```text
当前实际转速
诊断判定转速
表格预期转速
故障现场锁存值
```

---

## 第五阶段：风门与温度 AUTO（约 1 天）

### 15. `app_damper_config.h` 与 `app_damper.c`

先记住坐标：

```text
0步 = 全开 = 90°
1700步 = 全关 = 0°
正向 = 关闭
反向 = 打开
```

阅读路径：

```text
AppDamper_Init()
→ damper_start_boot_homing()
→ TIM6 AppDamper_TimerCallback() 每步换相
→ AppDamper_Process() 完成/保持/释放
```

再看绝对、相对、STOP、RELEASE 命令如何复用同一运动状态机。

### 16. `app_auto_control_config.h` 与 `app_auto_control.c`

先看 `evaluate_targets()` 的温度线性映射，再分开阅读：

```text
fan_control_process()
damper_control_process()
```

最后看 `AppAutoControl_SetMode()` 如何取得或释放执行机构控制权。

---

## 第六阶段：W2 上位机协议（约 1 天）

### 17. `app_uart.c`

不要从第 1 行一直读到末尾。按以下顺序跳转：

```text
宏：TYPE/OBJ/STATUS
→ AppUart_Init()
→ AppUart_RxCpltCallback()
→ AppUart_Process()
→ handle_byte()
→ dispatch_frame()
→ process_a1_query()/process_a2_control()
→ 任选一个对象深入
```

建议依次跟踪对象：

```text
0x02 LED（最简单）
0x01 PWM输出
0x05 NTC
0x07 风门
0x06 风机
0x08 单总线
0x09 AUTO
```

每个对象都检查四件事：长度、范围、业务调用、应答字段。

---

## 第七阶段：单总线协议栈（建议 1~2 天）

### 18. `app_onewire_protocol.h/.c`

先独立理解帧解析，不考虑业务：

```text
AA | source | destination | length | data | XOR
```

### 19. `app_onewire_uart.h/.c`

理解 UART 字节流如何进入 ring，异步发送如何报告完成，UART 错误后如何 re-arm RX。

### 20. `app_onewire.h/.c`

理解编译期角色门面：同一套上层 API 如何转发到 Master 或 Slave。

### 21. `app_onewire_master.h/.c`

建议先画两个维度：

```text
每从机上下文 contexts[]：地址、ONLINE/STALE、最后响应、最后结果
全局事务槽：busy、active_context、pending命令、master_state
```

跟踪一次“首次读取 0x03”：

```text
AppOneWireMaster_SubmitTo(0x03, READ, ...)
→ 创建context
→ begin_handshake()
→ 握手1
→ guard
→ 握手2
→ guard
→ READ
→ finish_success()
```

再跟踪 450 ms 内第二次读取，确认它跳过握手。

### 22. `app_onewire_slave.h/.c`

分别跟踪：握手1、握手2、写、读、500 ms 超时和外来地址帧。特别确认外来帧不刷新本机计时。

---

## 第八阶段：用测试反向验证理解（持续进行）

每读完一个模块，立即读对应测试：

```text
app_pwm.c                 -> Tests/test_app_pwm_output.c
app_pwm_input.c           -> Tests/test_app_pwm_input_dma.c
app_damper.c              -> Tests/test_app_damper_boot_homing.c
app_fan_health.c          -> Tests/test_app_fan_health.c
app_onewire_master.c      -> Tests/test_app_onewire_master.c
app_onewire_slave.c       -> Tests/test_app_onewire_slave.c
app_uart.c / 单总线对象  -> Tests/test_app_uart_onewire.c
```

测试中的 Arrange/Act/Assert 是最短的业务说明。尝试先预测断言，再运行测试。

---

## 推荐的 10 天阅读节奏

| 天数 | 内容 | 输出物 |
|---|---|---|
| 1 | build、main、config | 一张模块/外设表 |
| 2 | LED、PWM、NTC | 手算一个 PWM 和 NTC 例子 |
| 3 | ADC DMA、PWM Input | 两张数据流图 |
| 4 | BPF 与反馈仲裁 | 一张 FG 数据链图 |
| 5 | AppFan、手动转速 | 一张风机状态图 |
| 6 | 健康诊断 | 一张 settling/suspect/latch 图 |
| 7 | 风门 | 一张四拍相序和位置坐标图 |
| 8 | AUTO | 温度到 RPM/步数映射示例 |
| 9 | W2 | 任选三个对象逐字节解帧 |
| 10 | 单总线 Master/Slave | 首次读、快速再读、超时再读三条时序图 |

不要试图一次记住全部字段。目标是先能回答“数据从哪里来、在哪个状态变化、最终写到哪里”。
