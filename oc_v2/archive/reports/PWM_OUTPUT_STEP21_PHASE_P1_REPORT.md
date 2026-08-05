# Step21 通用PWM输出 Phase P1 实现报告

## 1. 目标

修复对象0x01在100 kHz、1%占空比下输出不稳定的问题，并在保持1%占空比分辨率的前提下扩大TIM4 PWM输出范围。本阶段不修改PE9/TIM1输入捕获算法、占空比定义或对象0x03协议。

## 2. 修改文件

- `Core/Inc/app_pwm.h`
- `Core/Src/app_pwm.c`
- `Core/Src/app_uart.c`
- `Core/Src/tim.c`
- `test.ioc`
- `Tests/stubs_pwm/tim.h`
- `Tests/test_app_pwm_output.c`
- `Tests/run_pwm_output_p1_host_test.sh`
- `Tests/test_app_uart_fan_manual.c`
- `Tests/test_app_uart_onewire.c`
- `oc_v2/`中的当前状态、协议、测试向量和归档索引

## 3. 原子配置

新增：

```c
bool AppPwm_Configure(bool enabled,
                      uint32_t frequency_hz,
                      uint16_t duty_x100);
```

对象0x01启用命令只调用一次该接口，不再依次调用频率、占空比、Enable三个会重复停启TIM4的函数。

## 4. 输出范围与分辨率

```text
频率范围：1 Hz～840000 Hz
占空比协议范围：0～10000（0.01%单位）
实际输出步进：1%（100 x100单位）
最小周期计数：100
```

量化规则为四舍五入到最接近的整数百分比，例如149→1%，150→2%。查询返回量化后的目标占空比和由CCR计算的实际占空比。

## 5. 100 kHz、1%

计算结果：

```text
TIM4 clock = 84 MHz
PSC = 0
ARR = 839
period counts = 840
CCR = 8
actual duty ≈ 0.95%
high pulse ≈ 95 ns
```

PB6 GPIO速度从`LOW`改为`VERY_HIGH`，并同步写入`test.ioc`。

## 6. 0%与100%

- 0%：TIM4_CH1强制非激活，PB6保持低；
- 100%：TIM4_CH1强制激活，PB6保持高；
- 1%～99%：PWM Mode 1。

这样避免依赖CCR边界值产生末端窄脉冲。

## 7. W2兼容性

对象0x01 DATA布局不变：

```text
01 | enable | frequency_hz LE32 | duty_x100 LE16
```

变化：

- enable=1频率上限改为840000 Hz；
- A2 DATA长度严格要求8字节；
- 占空比字段仍为0.01%单位，但输出按1%量化；
- A1响应布局不变；
- 禁用查询仍返回保存的目标占空比，保持既有语义。

重点命令：

```text
100 kHz / 1%: 7E A2 0A 01 01 A0 86 01 00 64 00 39
840 kHz / 1%: 7E A2 0A 01 01 40 D1 0C 00 64 00 2F
```

## 8. 输入捕获

本阶段未修改：

- `app_pwm_input.c/.h`；
- TIM1计数频率；
- ICFilter；
- 双边沿状态机；
- 占空比计算和反相语义；
- 对象0x03协议。

## 9. 测试

通过：

- 新增PWM输出host测试；
- 100 kHz/1%寄存器计算；
- 1 Hz和840 kHz边界；
- 1%量化；
- 0%/100%强制电平；
- 单次原子重启；
- 启动失败回滚；
- 对象0x01严格长度和范围；
- ASan/UBSan；
- 风机AUTO、BPF、手动定速、健康保护和W2全部既有回归。

当前环境没有`arm-none-eabi-gcc`和PowerShell，因此未执行真实Master/Slave ARM构建，也未生成HEX/BIN。

## 10. 硬件验证要求

示波器必须直接测量PB6：

1. 1 kHz：0%、1%、50%、99%、100%；
2. 100 kHz：1%、2%、50%、99%；
3. 200/500/840 kHz：1%、50%、99%；
4. 检查命令切换时是否仅出现一次配置空窗；
5. 记录频率、幅值、上升/下降时间和窄脉冲宽度。

840 kHz是软件计数分辨率上限，不代表已经保证外部引脚在1%时具有完整3.3 V脉冲。100 kHz/1%同样必须以示波器实测为准。
