# Step23 Phase P3 通用PWM输入DMA自动量程报告

更新时间：2026-07-28

## 1. 目标

在保持对象`0x03`W2布局和现有占空比语义不变的前提下，将PE9/TIM1通用PWM输入从Step22的双边沿HAL中断架构改为TIM1硬件PWM输入 + DMA批量采集，并把软件有效频率范围由1～20 kHz扩展到1～200 kHz。

本阶段不修改通用PWM输出对象`0x01`、风机、风门、单总线或上位机协议。

## 2. 修改文件

- `Core/Inc/app_pwm_input_config.h`
- `Core/Inc/app_pwm_input.h`
- `Core/Src/app_pwm_input.c`
- `Tests/test_app_pwm_input.c`
- `Tests/test_app_pwm_input_dma.c`
- `Tests/stubs_pwm_input/stm32f4xx_hal.h`
- `Tests/run_pwm_input_p3_host_test.sh`
- `oc_v2`中的当前状态、架构、测试、协议和归档索引文件

没有新增生产`.c`文件，因此`build.ps1`静态源文件列表无需修改。

## 3. 默认引擎与一行回退

Step23默认：

```c
#define APP_PWM_INPUT_ENGINE_MODE APP_PWM_INPUT_ENGINE_DMA_AUTORANGE
```

若目标板DMA路径出现异常，可在`app_pwm_input_config.h`改为：

```c
#define APP_PWM_INPUT_ENGINE_MODE APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY
```

即可恢复Step22的21 MHz双边沿HAL中断引擎，旧引擎仍通过独立回归测试。

## 4. DMA硬件路径

```text
PE9 / TIM1_CH1
→ CH1直接输入，下降沿作为周期边界和复位触发
→ CH2间接输入，捕获同一TI1上的上升沿
→ CCR1 = 周期
→ CCR2 = 现有语义下的external-high持续时间
→ CC1事件触发TIM1 DMA burst
→ DMA2 Stream6 Channel0循环搬运CCR1、CCR2
→ 主循环读取最新样本并取最近5组周期中位数
→ 对象0x03快照
```

DMA缓冲为128组，每组两个32位word：

```text
period, ext_high, period, ext_high, ...
```

不启用DMA中断，也不启用TIM1捕获中断。高频输入不会再按每个边沿进入HAL中断。

## 5. 占空比语义

用户已确认Step22占空比结果正确，本阶段保持其外部语义。

DMA模式使用：

- CH1下降沿：一周期边界；
- CH2上升沿：记录PE9物理低电平持续时间；
- `ext_high_ticks = CCR2`；
- `duty_x100 = ext_high_ticks / period_ticks`。

这等价于原引擎的：

```c
high = current_rise - previous_fall;
```

对象`0x03`的频率、占空比、状态和age字段含义均未改变。

## 6. 自动量程

TIM1为16位，采用三个运行时量程：

| 量程 | PSC | 计数时钟 | 主要用途 |
|---|---:|---:|---|
| FAST | 7 | 21 MHz | 高频，最高200 kHz |
| MEDIUM | 167 | 1 MHz | 中低频 |
| SLOW | 3359 | 50 kHz | 1 Hz低频 |

启动默认FAST。

- FAST发生真实计数溢出：切到MEDIUM；
- MEDIUM发生真实计数溢出：切到SLOW；
- SLOW周期小于200 tick：切到MEDIUM；
- MEDIUM周期小于200 tick：切到FAST。

TIM1设置`URS=1`，避免从模式复位事件被当作真实溢出。

## 7. 软件范围和分辨率

正式软件范围：

```text
1 Hz～200 kHz
```

FAST量程在200 kHz时：

```text
period = 21 MHz / 200 kHz = 105 tick
1%脉宽约1.05 tick
```

因此200 kHz、1%已经接近计数量化边界。实际可识别的最窄脉冲还受PE9电气边沿、输入滤波和信号源质量影响，必须实测。

输入数字滤波仍为`ICFilter=2`。

## 8. 主循环批处理

DMA循环持续运行，主循环通过DMA剩余计数读取最新生产位置。

- 不要求主循环消费每一个周期；
- 即使USART1阻塞导致DMA缓冲多次环回，也读取当前最新样本；
- 最近最多5个有效周期按period排序，选中位样本发布；
- 单个周期毛刺不会直接覆盖稳定结果；
- DMA传输错误会记录并重启当前量程。

## 9. 内部诊断字段

`AppPwmInputSnapshot`末尾新增仅供调试器使用的字段：

```text
engine_mode
range_profile
dma_buffer_pairs
dma_sample_count
profile_switch_count
dma_error_count
```

对象`0x03`没有传输这些字段，线上DATA仍固定11字节。

## 10. 测试

新增测试覆盖：

- 200 kHz / 约1%；
- 最近5周期中位数抑制单次周期毛刺；
- FAST→MEDIUM→SLOW降档；
- SLOW→MEDIUM→FAST升档；
- 1 Hz / 25%；
- 静态高超时；
- DMA错误恢复；
- Step22旧引擎一行回退编译与行为；
- ASan/UBSan。

结果：

```text
general PWM input DMA autorange phase-P3 tests passed
general PWM input phase-P2 tests passed
general PWM input step-23 phase-P3 host verification passed
```

同时通过PWM输出、风机BPF、AUTO、手动定速和健康保护回归测试。

使用Clang ARM目标对以下文件执行严格语法检查并通过：

- `app_pwm_input.c`
- `app_uart.c`
- `main.c`
- `tim.c`
- `stm32f4xx_it.c`

当前环境仍没有`arm-none-eabi-gcc`，因此没有完成真实Master/Slave链接和HEX生成。

## 11. CubeMX维护说明

`MX_TIM1_Init()`仍提供Step22基础输入捕获配置。Step23的PWM Input、DMA burst和自动量程寄存器由`AppPwmInput_Init()`在USER应用层运行时完整接管。

本阶段没有尝试让CubeMX表达运行时三档PSC自动切换。重新生成后必须保留：

- PE9仍为TIM1_CH1；
- TIM1时钟源仍为内部时钟；
- DMA2可用；
- `app_pwm_input.c`仍由构建脚本编译。

## 12. 硬件验证计划

PB6接PE9并共地，先验证：

```text
1 Hz、10 Hz、100 Hz
1 kHz、5 kHz、20 kHz
50 kHz、100 kHz、150 kHz、200 kHz
```

每个频率测试：

```text
1%、10%、25%、50%、75%、99%
```

重点观察调试器字段：

```text
status
freq_millihz
duty_x100
period_ticks
ext_high_ticks
range_profile
profile_switch_count
dma_error_count
```

必须额外验证：

- 频率跨100 Hz、5 kHz附近时自动量程是否平滑；
- 200 kHz连续运行时USART1、单总线、风机和风门是否正常；
- 静态高/低2.5秒识别；
- 200 kHz、1%是否被PE9可靠识别；
- 若异常，使用一行宏回退Step22引擎。

## 13. 尚未验证的风险

- DMA2 Stream6 Channel0和TIM1 DMA burst尚未在当前目标板实测；
- 200 kHz、1%只有约一个计数，电气脉宽容差较大；
- 三档切换会产生短暂SEARCH窗口；
- 当前W2没有暴露量程和DMA错误诊断，需要调试器观察；
- 真实ARM GCC构建和目标板长期测试尚未完成。
