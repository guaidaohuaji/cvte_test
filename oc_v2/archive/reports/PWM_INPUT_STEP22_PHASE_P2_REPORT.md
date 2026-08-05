# Step22 Phase P2 通用PWM输入基础扩展报告

日期：2026-07-28

## 1. 目标

在不改变现有对象`0x03`占空比计算语义和W2布局的前提下，提高TIM1输入捕获的计数分辨率和可用频率范围，为后续更高频率DMA捕获阶段建立稳定基线。

本阶段正式软件范围：

- 频率：1 Hz～20 kHz；
- 输入：PE9 / TIM1_CH1；
- 双边沿中断捕获；
- 静态高、静态低超时识别保持；
- 对象`0x03`响应格式保持不变。

## 2. 修改文件

### 新增

- `Core/Inc/app_pwm_input_config.h`
- `Tests/stubs_pwm_input/stm32f4xx_hal.h`
- `Tests/stubs_pwm_input/tim.h`
- `Tests/test_app_pwm_input.c`
- `Tests/run_pwm_input_p2_host_test.sh`
- 本报告与测试日志

### 修改

- `Core/Src/app_pwm_input.c`
- `Core/Src/tim.c`
- `test.ioc`
- `oc_v2`当前状态、协议、源码索引和测试说明

未修改：

- `app_pwm_input.h`线上快照字段；
- `app_uart.c`对象`0x03`布局；
- 输入边沿状态机；
- 周期和占空比公式；
- `INPUT_INVERTED`语义；
- 对象`0x01`、`0x02`及`0x04～0x09`协议。

## 3. TIM1配置变化

| 项目 | Step21 | Step22 |
|---|---:|---:|
| TIM1输入时钟 | 168 MHz | 168 MHz |
| PSC | 167 | 7 |
| 计数时钟 | 1 MHz | 21 MHz |
| ARR | 65535 | 65535 |
| ICFilter | 15 | 2 |
| 软件频率范围 | 1～5 kHz | 1～20 kHz |
| 超时 | 2500 ms | 2500 ms |

`test.ioc`已同步为PSC=7、ICFilter=2，避免CubeMX重新生成恢复旧值。

## 4. 选择21 MHz和20 kHz的原因

TIM1时钟168 MHz，PSC=7后：

```text
168 MHz / (7 + 1) = 21 MHz
```

在20 kHz时：

```text
周期计数 = 21,000,000 / 20,000 = 1050 ticks
1%周期约 = 10.5 ticks
```

该分辨率明显高于原来的1 MHz计数时钟。ICFilter=2采用较短的4采样确认，可通过20 kHz下的窄脉冲，同时仍保留基本毛刺抑制。

20 kHz输入会产生约40,000次捕获中断/秒。当前仍经过HAL中断路径，因此本阶段不把50 kHz或100 kHz列入正式范围。继续扩展应改为TIM1捕获DMA和主循环批处理。

## 5. 占空比算法确认

根据用户确认，本阶段明确保持以下现有逻辑不变：

```c
period = current_rise - last_rise;
high   = current_rise - last_fall;
```

以及：

```c
#if INPUT_INVERTED
    ext_high = period - high;
#else
    ext_high = high;
#endif
```

新增回归测试固定了当前语义，防止未来高频架构改造时意外改变对象`0x03`的占空比结果。

## 6. 初始化状态清理

审计发现重新初始化时，旧代码没有完整清除：

- `last_rise_ts` / `last_fall_ts`；
- `raw_period_ticks` / `raw_high_ticks`；
- `raw_last_cap_ms`；
- `cached_snap`其他字段。

Step22在`AppPwmInput_Init()`中完整清零这些状态，避免软件重初始化后继承旧捕获时间和旧快照。该修正不改变正常连续运行算法。

## 7. W2协议

对象`0x03`查询仍为：

```text
7E A1 03 03 A7
```

成功响应DATA仍为11字节：

```text
[0] status
[1] object=0x03
[2] input_state
[3..6] frequency_millihz LE32
[7..8] duty_x100 LE16
[9..10] age_ms LE16
```

没有新增字段、选择器或状态码。

## 8. 主机测试

新增测试覆盖：

1. PSC=7、21 MHz、ICFilter=2配置契约；
2. 20 kHz / 50%捕获；
3. 当前占空比语义保持；
4. 高于20 kHz输入被拒绝；
5. CC1 overcapture后重新同步；
6. 2500 ms静态高电平识别；
7. HAL输入捕获启动失败；
8. AddressSanitizer和UndefinedBehaviorSanitizer。

结果：

```text
general PWM input phase-P2 tests passed
general PWM input step-22 phase-P2 host verification passed
```

Step21 PWM输出、风机BPF、AUTO、手动定速和健康保护回归测试全部通过。

## 9. ARM构建状态

当前执行环境未安装：

- `arm-none-eabi-gcc`；
- PowerShell。

因此没有执行：

```powershell
.\build.ps1 -Role master
.\build.ps1 -Role slave
```

也没有生成新的HEX/BIN。需要在Windows工程环境完成双角色构建。

## 10. 实物验证步骤

建议先将PB6临时连接到PE9，并共地。使用Step21对象`0x01`生成标准PWM，再查询对象`0x03`。

查询输入：

```text
7E A1 03 03 A7
```

推荐输出测试帧：

```text
1 kHz / 50%:
7E A2 0A 01 01 E8 03 00 00 88 13 34

5 kHz / 25%:
7E A2 0A 01 01 88 13 00 00 C4 09 16

10 kHz / 50%:
7E A2 0A 01 01 10 27 00 00 88 13 80

20 kHz / 50%:
7E A2 0A 01 01 20 4E 00 00 88 13 B7

20 kHz / 1%:
7E A2 0A 01 01 20 4E 00 00 64 00 80
```

建议验证：

- 1、5、10、20 kHz；
- 1%、10%、25%、50%、75%、99%；
- 静态低和静态高；
- 连续运行10分钟是否出现`ovc_count`增加或状态掉回SEARCH；
- 20 kHz时主循环、USART1、风机反馈和单总线是否仍稳定。

## 11. 下一阶段边界

本阶段没有宣称20 kHz以上可用。若20 kHz实物验证通过，下一阶段建议：

```text
TIM1捕获DMA
→ 时间戳缓冲
→ 主循环批量解析
→ 继续保持当前周期和占空比语义
```

目标可再评估50～200 kHz。禁止只提高`APP_PWM_INPUT_MAX_FREQ_HZ`而不处理双边沿中断负载。
