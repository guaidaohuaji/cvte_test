# 构建、下载和最低回归

## 1. 构建

```powershell
cd C:\Users\user\Desktop\document\project\cvte_test-step11-led-arbitration\cvte_test-step11-led-arbitration

.\build.ps1 -Role master
.\build.ps1 -Role slave
```

`build.ps1`静态列出所有模块，包括：

```text
app_pwm
app_pwm_input
app_ntc
app_fan
app_adc_scan
app_fan_feedback_adc
app_onewire_*
app_led
app_uart
```

新增`.c`文件必须加入`$C_FILES`。
当前新增风门模块后的 V3 最终构建结果（Step 5.1）：

Master: text=48428, data=384, bss=6088, total=54900, 0 error 0 new warning
Slave:  text=44132, data=384, bss=6584, total=51100, 0 error 0 new warning

## 2. 构建产物检查

```powershell
Get-Item .\build_master\test_master.elf
Get-Item .\build_master\test_master.hex
Get-Item .\build_slave\test_slave.elf
Get-Item .\build_slave\test_slave.hex

Get-FileHash .\build_master\test_master.hex -Algorithm SHA256
Get-FileHash .\build_slave\test_slave.hex -Algorithm SHA256
```

两个角色HEX应不同。

## 3. 角色与单总线最低回归

按V1流程：

1. A板确认`role=01`
2. B板确认`role=02`
3. 写`0x0008=0x1234`
4. 等待超过500 ms
5. 读`0x0008`
6. 确认返回`0x1234`

## 4. 既有功能最低烟雾测试

USART1均使用9600、8N1、HEX。

### 查询所有对象

```text
7E A1 03 01 A5
7E A1 03 02 A6
7E A1 03 03 A7
7E A1 03 04 A8
7E A1 03 05 A9
7E A1 03 06 AA
7E A1 03 08 AC
```

每条至少应返回合法W2帧，不应导致系统复位或阻塞。

### 通用PWM

- 发送1 kHz、50%；
- PB6示波器确认；
- 再disable确认保持低；
- 再enable确认恢复。

### PWM输入

- 向PE9输入1 kHz、25%和75%；
- 查询对象0x03；
- 检查频率和占空比；
- 断开信号超过2.5 s，检查静态高/低状态。

### NTC

- 查询对象0x05；
- 检查状态、ADC、电压、电阻和温度；
- 使用已知电阻或温箱验证；
- 不仅判断状态OK，还要检查数值误差。

### 风机

- 先100%启动；
- 确认PB8 10 kHz；
- 检查2 s boost；
- 检查反馈频率和RPM；
- 停止反馈后检查500 ms无测速；
- 再恢复反馈检查状态恢复。

### PD13和LED

- PD13高/低读回；
- LED手动亮/灭；
- 恢复自动；
- 验证PE8低有效。

## 5. 修改模块与回归范围

| 修改模块 | 强制回归 |
|---|---|
| `app_adc_scan`/ADC/TIM2/DMA | NTC + 风机反馈同时回归 |
| `app_fan_feedback_adc` | 风机RPM + ADC overrun |
| `app_fan`/TIM10 | 风机PWM、boost、NO_TACH |
| `app_pwm_input`/TIM1 | 频率、占空比、静态电平、溢出 |
| `app_pwm`/TIM4 | 多频率、多占空比、disable/enable |
| `app_uart` | 对象0x01~0x08全部回归 |
| `main.c`回调 | UART、TIM、ADC全部回归 |
| 单总线模块 | Master和Slave双构建、双板读写 |

## 6. 停止规则

- Master构建失败：停止；
- Slave构建失败：停止；
- 任何旧对象回归失败：停止；
- 不自动进行下一轮修复；
- 先报告错误和影响，再等待用户批准。


## Step21 通用PWM输出 Phase P1 主机测试

```text
Tests/run_pwm_output_p1_host_test.sh
```

覆盖：

- 1 Hz与840 kHz边界；
- 100 kHz/1%的PSC、ARR、CCR；
- 1%占空比量化；
- 0%/100%强制电平；
- 一次W2命令只调用一次原子配置；
- 严格8字节A2长度；
- 启动失败回滚；
- ASan/UBSan。

目标板验证必须用示波器直接测PB6，不要仅依赖PE9输入捕获结果。


## Step22 通用PWM输入 Phase P2 主机测试

```text
Tests/run_pwm_input_p2_host_test.sh
```

覆盖：

- TIM1 21 MHz计数配置契约；
- 20 kHz / 50%捕获；
- 原有占空比计算语义保持；
- 高于20 kHz拒绝；
- CC1 overcapture重新同步；
- 2500 ms静态电平识别；
- 输入捕获启动失败；
- ASan/UBSan。


## Step23 通用PWM输入 Phase P3 主机测试

```text
Tests/run_pwm_input_p3_host_test.sh
```

覆盖：

- 200 kHz / 约1% DMA捕获；
- 1 Hz / 25%慢档捕获；
- 最近5周期中位数抑制单次周期毛刺；
- FAST/MEDIUM/SLOW自动升降档；
- 静态高超时；
- DMA错误恢复；
- Step22旧中断引擎一行回退；
- ASan/UBSan；
- Clang ARM目标严格语法检查。

目标板回环建议逐级测试1 Hz、10 Hz、100 Hz、1 kHz、5 kHz、20 kHz、50 kHz、100 kHz、150 kHz、200 kHz，并覆盖1%、10%、50%、90%、99%。
