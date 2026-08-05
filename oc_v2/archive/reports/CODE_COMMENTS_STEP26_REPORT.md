# Step26 详细注释与代码阅读资料实施报告

## 1. 目标

本版本以 Step25 单总线多从机工程为基线，只增加教学性注释和学习资料，帮助首次接触工程的开发者理解：

- 裸机主循环与 HAL 回调的分工；
- 每个应用模块的输入、输出、硬件资源和调用上下文；
- PWM、ADC、风机、风门、AUTO、W2 和单总线状态机；
- 从配置、接口、实现到测试的推荐阅读顺序；
- 调试时应观察的变量、断点位置和常见修改规则。

## 2. 注释范围

已对以下自研应用代码增加详细中文注释：

```text
Core/Src/app_*.c       18个实现文件
Core/Inc/app_*.h       26个接口/配置文件
Core/Src/main.c        初始化、主循环和HAL回调说明
build.ps1              Master/Slave构建角色和输出目录说明
```

注释内容包括：

- 每个文件的模块职责、数据输入、数据输出、执行上下文和阅读重点；
- 360个函数定义的 Doxygen 风格说明；
- 关键数学公式、DMA数据布局、状态转换和失败回滚；
- 中断与主循环共享数据的说明；
- 目标值、实际值、快照值和锁存现场的区别；
- Master多从机上下文与全局物理事务槽的区别。

未对 `Drivers/` 中的 ST HAL/CMSIS 库源码逐行添加注释，也未大范围改写 CubeMX 生成的 `gpio.c/tim.c/adc.c/dma.c/usart.c`。原因是这些文件由厂商或 CubeMX 维护，人工注释会显著增加噪声并可能在重新生成时丢失。学习资料中已经说明应如何阅读这些生成文件。

## 3. 新增学习资料

```text
oc_v2/reference/learning/00_CODE_READING_GUIDE.md
oc_v2/reference/learning/01_ARCHITECTURE_AND_DATAFLOW.md
oc_v2/reference/learning/02_STATE_MACHINE_INDEX.md
oc_v2/reference/learning/03_DEBUGGING_AND_STUDY_GUIDE.md
oc_v2/reference/learning/04_GLOSSARY_AND_UNITS.md
```

其中 `00_CODE_READING_GUIDE.md` 提供从简单模块到复杂协议栈的分阶段顺序，并给出10天学习计划。

## 4. 文档同步

- 更新 `oc_v2/03_SOFTWARE_ARCHITECTURE.md` 为当前 Step25/26 真实架构；
- 在 `00_READ_ME_FIRST.md` 和 `09_SOURCE_INDEX.md` 增加学习版入口；
- 更新 `13_DOCUMENT_ARCHIVE_INDEX.md`；
- 更新 `manifest.json` 与 `project_state.yaml`，明确 Step26 是注释学习版，执行行为仍为 Step25。

## 5. 行为不变验证

使用注释感知的词法归一化脚本，移除 C/C++ 注释和空白后比较 Step25 与 Step26：

```text
61个 Core/*.c 和 Core/*.h 文件的非注释 token 流完全一致
```

这意味着：

- 没有改变任何表达式、常量、条件或状态赋值；
- 没有改变 W2 或单总线帧格式；
- 没有改变定时器、ADC、DMA、GPIO 和 UART 配置；
- 没有改变 Master/Slave 角色或从机地址处理；
- 没有改变风机、风门、AUTO 和健康诊断行为。

## 6. 测试

以下11个现有主机测试脚本全部通过：

```text
run_damper_boot_homing_host_test.sh
run_fan_auto_host_test.sh
run_fan_bpf_host_test.sh
run_fan_health_phase2_host_test.sh
run_fan_health_phase3_host_test.sh
run_manual_fan_phase1_host_test.sh
run_manual_fan_phase2_host_test.sh
run_onewire_multislave_host_test.sh
run_pwm_input_p2_host_test.sh
run_pwm_input_p3_host_test.sh
run_pwm_output_p1_host_test.sh
```

详细输出见：

```text
oc_v2/archive/test_logs/CODE_COMMENTS_STEP26_TEST_LOG.txt
```

## 7. 阅读入口

建议第一份打开：

```text
oc_v2/reference/learning/00_CODE_READING_GUIDE.md
```

随后按以下顺序开始：

```text
build.ps1
→ Core/Src/main.c
→ Core/Inc/*_config.h
→ app_led
→ app_pwm
→ app_ntc/app_adc_scan
→ app_pwm_input
→ 风机数据链
→ app_damper/app_auto_control
→ app_uart
→ app_onewire_protocol/uart/master/slave
→ 对应Tests
```

## 8. 尚未执行

当前环境没有 ARM 交叉编译器和目标板，因此没有执行真实 `build.ps1`、J-Link 下载和硬件回归。由于非注释 token 完全一致，Step25 已验证过的固件逻辑没有发生源代码变化；但正式交付前仍建议在你的 Windows/VSCode 环境分别构建 Master、Slave 0x02 和 Slave 0x03。
