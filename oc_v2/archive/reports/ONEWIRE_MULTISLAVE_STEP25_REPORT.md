# Step25 单总线多从机按需握手实施报告

更新时间：2026-07-28  
工程：`cvte_test-step25-onewire-multislave`

## 1. 实施目标

在保留原单总线500 ms失效规则的前提下，将固定单从机架构改为一主多从：

- 主机地址固定为`0x01`；
- 从机地址范围为`0x02～0xFE`；
- 主机上电不自动握手；
- 不扫描、不轮询、不发送后台保活；
- 上位机提交读写时，主机只检查目标从机的握手有效性；
- 目标上下文仍在450 ms有效期内时直接读写；
- 已失效或首次访问时先完成两次握手，再自动继续原读写；
- 每个从机仍在500 ms未收到发给本机的正确帧后进入通信故障；
- 发给其他地址的帧必须静默忽略，不能应答，也不能刷新500 ms计时。

## 2. 主机状态机改造

`app_onewire_master.c`已从单一固定从机状态改为：

```text
最多8个独立从机上下文
+
一个共享物理总线事务槽
```

每个上下文独立保存：

- 从机地址；
- 链路状态；
- 最近一次有效响应时间；
- 最近操作；
- 操作结果；
- 寄存器地址和值。

主机初始化后处于`APP_ONEWIRE_MASTER_IDLE`，不在USART6上发送任何启动握手帧。默认地址`0x02`上下文仅预创建为离线状态，不代表已经握手。

提交读写后的处理流程：

```text
目标上下文ONLINE且响应年龄<450 ms
    → 直接发送读写
否则
    → 握手1
    → 握手2
    → 自动继续原读写
```

共享总线忙时不接收第二个事务，避免两个从机事务交叉。第一版不实现命令队列。

## 3. 从机地址与静默过滤

从机地址由编译宏`APP_ONEWIRE_SLAVE_ADDRESS`确定，合法范围为`0x02～0xFE`。

从机只处理目标地址等于自身地址的帧。其他目标地址的完整合法帧会：

- 静默忽略；
- 不发送响应；
- 不推进握手状态；
- 不刷新本机500 ms链路计时。

新增诊断计数`ignored_foreign_frame_count`，用于观察总线上收到的非本机帧数量。

## 4. 构建方式

`build.ps1`新增可选参数`-SlaveAddress`：

```powershell
.\build.ps1 -Role master
.\build.ps1 -Role slave -SlaveAddress 0x02
.\build.ps1 -Role slave -SlaveAddress 0x03
```

不同从机地址输出到独立目录：

```text
build_slave_02/
build_slave_03/
```

避免不同地址的HEX、BIN和MAP互相覆盖。

## 5. W2对象0x08兼容扩展

### A1查询

旧格式保持：

```text
08
```

Master默认查询`0x02`。

新格式：

```text
08 | slave_address
```

查询`0x03`：

```text
7E A1 04 08 03 B0
```

查询只读取主机内存中的上下文，不会触发USART6握手、扫描或保活。

A1成功响应仍为16字节DATA。原保留字段定义为：

```text
d[12]    slave_address
d[13]    context_valid
d[14..15] last_response_age_ms LE16
```

`0xFFFF`表示从未收到该从机的有效响应。

### A2控制

旧6字节格式继续默认访问`0x02`：

```text
08 | operation | address_LE16 | value_LE16
```

新增7字节多从机格式：

```text
08 | slave_address | operation | address_LE16 | value_LE16
```

示例：

```text
重新握手0x03：       7E A2 09 08 03 01 00 00 00 00 B7
写0x03 0008=1234：  7E A2 09 08 03 03 08 00 34 12 07
读0x03 0008：       7E A2 09 08 03 06 08 00 00 00 C4
```

READ/WRITE只需提交一次。是否需要握手以及握手成功后继续原操作，全部由主机MCU完成。

## 6. 修改文件

固件：

- `Core/Inc/app_onewire_config.h`
- `Core/Inc/app_onewire.h`
- `Core/Inc/app_onewire_master.h`
- `Core/Inc/app_onewire_slave.h`
- `Core/Src/app_onewire.c`
- `Core/Src/app_onewire_master.c`
- `Core/Src/app_onewire_slave.c`
- `Core/Src/app_uart.c`
- `build.ps1`

测试：

- `Tests/run_onewire_multislave_host_test.sh`
- `Tests/test_app_onewire_master.c`
- `Tests/test_app_onewire_slave.c`
- `Tests/test_app_onewire_role.c`
- `Tests/test_app_uart_onewire.c`
- 单总线底层及既有协议回归测试适配文件。

文档：

- `oc_v2/reference/protocol/STM32_W2_step25_通讯协议_OpenCode生成解析规范.txt`
- `oc_v2/`当前状态、功能、测试向量、索引和机器可读状态文件。

## 7. 测试结果

Step25专项host测试通过：

- 主机上电无自动握手；
- 首次读写先握手再继续；
- 450 ms内直接读写；
- 超时后重新握手；
- `0x02`和`0x03`上下文隔离；
- 某个从机握手失败不污染其他上下文；
- 共享物理总线BUSY；
- 最多8个上下文；
- 从机仅响应自身地址；
- 外来地址帧不刷新500 ms计时；
- W2旧格式与新格式；
- AddressSanitizer和UndefinedBehaviorSanitizer。

完整既有回归也通过：

- Step24风门上电全开校准；
- Step21通用PWM输出；
- Step22输入捕获回退；
- Step23 PWM Input DMA自动量程；
- 风机BPF、AUTO、手动定速和健康保护；
- 单总线协议层和UART底层测试。

完整日志：`oc_v2/archive/test_logs/ONEWIRE_MULTISLAVE_STEP25_TEST_LOG.txt`。

## 8. 未完成验证与限制

当前环境没有`arm-none-eabi-gcc`和PowerShell，因此Step25未执行真实Master/Slave ARM链接，也没有生成新的ELF、HEX、BIN和MAP。

以下内容仍需目标板验证：

- 一主两从或更多节点实际并联；
- 每块从机编译地址是否与烧录对象一致；
- 非目标从机是否始终保持静默；
- 多节点负载下38400 baud波形；
- 某从机掉线时其他从机是否正常；
- 450/500 ms边界时序；
- TX驱动释放、上拉、电容和总线冲突。

主机最多保存8个从机上下文；地址空间虽然是`0x02～0xFE`，但一次运行中最多访问8个不同地址。没有广播、自动扫描、后台轮询和命令队列。

## 9. 上位机状态

现有上位机v0.1.8仍可使用旧格式访问默认从机`0x02`，但不能选择其他从机地址，也不会显示新增的地址、上下文有效性和响应年龄字段。

要完整使用多从机功能，下一阶段需要更新上位机：

- 增加目标从机地址选择；
- 使用对象0x08新7字节A2格式；
- 使用“08 | slave_address”查询指定上下文；
- 显示目标地址、上下文有效性和响应年龄；
- 保持旧`0x02`模式兼容；
- 上位机仍不负责握手和保活，只提交一次读写。
