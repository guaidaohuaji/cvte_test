# 当前开发状态和证据等级

## 1. 已硬件验证：单总线核心链路

实际记录证明：

```text
Master提交写0x0008=0x1234
→ result SUCCESS
→ 数十秒后提交读
→ 自动重新握手
→ 读回0x1234
```

这证明：

- Master角色运行；
- Slave握手和响应；
- 写入；
- RAM寄存器保存；
- 链路过期；
- 自动重新握手；
- 读取；
- A1快照；
- STALE不覆盖最近事务结果。

## 2. 已有主机测试/仿真

已保存记录：

```text
app_onewire_protocol
app_onewire_uart
app_onewire_slave
app_onewire_master
app_onewire role dispatch
app_led arbitration
app_uart one-wire object
master/slave endpoint simulation
```

## 3. 源码存在但本交接未保存硬件验证证据

| 功能 | 源码状态 | 本交接证据 |
|---|---|---|
| 通用PWM输出 | 已实现 | 未保存示波器测试 |
| PWM输入频率 | 已实现 | 未保存标准信号源测试 |
| PWM输入占空比 | 已实现 | 未保存标准信号源测试 |
| 风机PWM和boost | 已实现 | 未保存实际风机测试 |
| 风机反馈频率/RPM | 已实现 | 未保存转速仪对照 |
| NTC温度 | 已实现 | 未保存温箱/电阻箱对照 |
| PD13输出 | 已实现 | 未保存外部负载测试 |
| PE8手动和自动灯 | 已实现并有主机单测 | 未保存全部硬件波形 |
| TB6612风门驱动 | 已实现(Step 1~5.1) | 未保存任何硬件测试 |

## 4. 既有功能没有专用单元测试

第11步`Tests/`中没有针对以下模块的独立测试：

```text
app_pwm.c
app_pwm_input.c
app_adc_scan.c
app_ntc.c
app_fan.c
app_fan_feedback_adc.c
app_damper.c
```

所以OpenCode不得把“源码已实现”写成“测试全部通过”。

## 5. 当前建议的黄金基线

在继续重大开发前，应归档：

- Master ELF/HEX/BIN/MAP；
- Slave ELF/HEX/BIN/MAP；
- 两个HEX SHA256；
- `arm-none-eabi-size`；
- 已知成功单总线日志；
- 通用PWM示波器截图；
- PWM输入标准信号测试；
- NTC至少3个参考点；
- 风机启停、boost和RPM对照。

## 6. 当前功能状态摘要

```text
单总线核心功能          硬件通过
对象0x08               硬件通过
对象0x07               源码存在, 构建通过, 待硬件验证
对象0x01~0x06          源码存在，需重新建立验证档案
PE8仲裁                源码与主机单测存在
TX异常恢复             静态风险，未发生
既有模拟采集链路        源码存在，需硬件回归
风门驱动               源码存在, 构建通过, 待硬件验证
