# Step24 风门上电全开校准实施报告

日期：2026-07-28  
基线：`cvte_test-step23-pwm-input-dma-p3.zip`  
交付：`cvte_test-step24-damper-boot-open`

## 1. 目标

Master构建每次上电或复位后，不等待人工设置当前位置，立即沿机械打开方向无条件运行1700步。运动完成后，把机械全开端建立为逻辑`0步/90°`，从而允许后续直接使用绝对位置和自动温控控制。

当前实物方向定义保持不变：

- 正向相序：关闭；
- 反向相序：打开；
- 0步：全开；
- 1700步：全关。

## 2. 实现行为

`AppDamper_Init()`确认TIM6可用后立即启动上电校准：

```text
position_valid = false
last_command = BOOT_HOMING
state = BOOT_HOMING
沿打开方向运行1700步
```

当前`APP_DAMPER_FORWARD_IS_OPEN=0`，因此实际使用反向相序。

校准期间：

- 对象0x07状态为`0x08 BOOT_HOMING`；
- `flags.bit5=1`；
- `position_valid=0`；
- 绝对移动、相对移动、设置当前位置均返回`BUSY`；
- 自动温控因位置无效停留在`WAIT_POSITION`，不会抢占风门；
- STOP、RELEASE和紧急停机仍可中止动作。

第1700步完成后：

```text
current_steps = 0
target_steps = 0
position_valid = true
state = POST_MOVE_HOLD
```

保持最后相位100ms后释放电机并进入`IDLE_RELEASED`。

若校准未完成即被STOP、RELEASE或紧急停机中断，则不会建立位置参考，`position_valid`保持无效。

## 3. W2兼容性

对象0x07的A1 DATA仍为23字节，A2子命令和长度均未改变。

新增枚举和标志：

- `damper_state=0x08`：BOOT_HOMING；
- `flags.bit5`：boot_homing_active；
- `last_command=0x03`：BOOT_HOMING，仅表示固件内部启动动作，不是新的A2控制子命令。

旧上位机仍可按原23字节结构解析，但若没有新枚举映射，校准期间可能显示“未知状态0x08”。

## 4. 修改文件

固件：

- `Core/Inc/app_damper.h`
- `Core/Inc/app_damper_config.h`
- `Core/Src/app_damper.c`

测试：

- `Tests/test_app_damper_boot_homing.c`
- `Tests/run_damper_boot_homing_host_test.sh`
- `Tests/stubs_damper/main.h`
- `Tests/stubs_damper/tim.h`
- `Tests/run_pwm_input_p2_host_test.sh`：补充旧中断引擎宏，修复Step23后该历史测试入口无法单独编译的问题；不改变固件默认PWM输入引擎。

文档：

- `oc_v2/00_READ_ME_FIRST.md`
- `oc_v2/04_PROTOCOL_AND_COMMANDS.md`
- `oc_v2/05_CURRENT_STATUS_AND_EVIDENCE.md`
- `oc_v2/09_SOURCE_INDEX.md`
- `oc_v2/10_EXISTING_APPLICATION_FEATURES.md`
- `oc_v2/13_DOCUMENT_ARCHIVE_INDEX.md`
- `oc_v2/project_state.yaml`
- `oc_v2/manifest.json`
- `oc_v2/reference/protocol/STM32_W2_step24_通讯协议_OpenCode生成解析规范.txt`

## 5. 测试覆盖

Step24专项测试覆盖：

- 初始化立即启动反向1700步；
- 校准状态、方向、剩余步数和标志；
- 校准期间普通位置命令返回BUSY；
- 第1700步建立0步全开参考；
- 100ms保持后释放；
- 校准完成后绝对移动到1700步使用正向关闭；
- STOP中止不建立参考；
- RELEASE中止不建立参考；
- 紧急停机中止并进入FAULT；
- Master专项执行；
- Slave角色告警级编译检查；
- AddressSanitizer；
- UndefinedBehaviorSanitizer。

全部现有host测试入口均通过，包括风机AUTO、BPF、健康保护、手动定速、PWM输出、PWM输入P2回退和PWM输入P3 DMA。

完整输出见：

`oc_v2/archive/test_logs/DAMPER_BOOT_OPEN_STEP24_TEST_LOG.txt`

## 6. 未完成验证

当前环境没有`arm-none-eabi-gcc`、PowerShell和目标板，因此未执行：

- `build.ps1 -Role master/slave`真实ARM构建；
- ELF/HEX/BIN/MAP生成；
- 目标板烧录；
- 实物打开方向确认；
- 300 PPS下1700步约5.67秒的实际运行确认；
- 已处于全开位置时持续撞机械端的电气与热行为确认。

## 7. 目标板验收步骤

1. 将风门置于中间位置，复位Master板；
2. 确认风门立即向打开方向动作；
3. 查询对象0x07，运动期间应看到`state=0x08`、`position_valid=0`、`remaining_steps`递减；
4. 约5.67秒后应进入保持，再在100ms后释放；
5. 最终查询应为`current_steps=0`、`target_steps=0`、`position_valid=1`；
6. 发送绝对位置1700，确认风门向关闭方向移动；
7. 在上电校准中途发送STOP，确认停止后位置无效；
8. 再次复位，确认重新执行完整上电校准。
