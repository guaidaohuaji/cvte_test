# 文档归档与证据索引

本文件说明工程中的说明文档、测试日志和协议参考资料的统一存放位置。

## 整理原则

- 工程根目录只保留编译、配置、源码、测试脚本和工具入口文件。
- 项目说明、阶段报告、测试日志、Sanitizer 日志和协议规范统一放入 `oc_v2/`。
- 历史报告与日志不参与固件编译，也不影响 Keil、Makefile 或 `build.ps1`。
- `oc_v2.zip` 是 `oc_v2/` 当前内容的同步副本，交付 OpenCode 时优先使用文件夹；需要单文件传递时使用 ZIP。

## 目录结构

```text
oc_v2/
├─ 00_READ_ME_FIRST.md ... 12_V2_SOURCE_EXTRACTION_NOTES.md
├─ 13_DOCUMENT_ARCHIVE_INDEX.md
├─ archive/
│  ├─ reports/                 历次功能阶段报告
│  └─ test_logs/               主机测试与 Sanitizer 日志
└─ reference/
   ├─ protocol/                W2 通讯协议详细规范
   ├─ release/                 发布说明与固件产物说明
   └─ filter_design/           风机反馈滤波器设计验证资料
```

## 当前协议规范

- `reference/protocol/STM32_W2_step27_通讯协议_OpenCode生成解析规范.txt`

Step20版本保留用于历史追溯。

## 当前发布说明

- `reference/release/NO_FIRMWARE_BINARIES.txt`

## 风机滤波器设计资料

- `reference/filter_design/fan_bpf_frequency_response.md`

## 阶段报告

`archive/reports/` 中保留：

- 风机 BPF Stage 1～3 报告；
- 自动风机快速调节 Step15 报告；
- 手动目标转速 Phase 1～2 报告；
- 风机健康检测 Step18～Step20 报告；
- 通用PWM输出 Step21 Phase P1 报告；
- 通用PWM输入 Step22 Phase P2、Step23 Phase P3，以及风门Step24上电全开校准、单总线Step25多从机按需握手报告。

## 测试证据

`archive/test_logs/` 中保留各阶段主机测试、回归测试和 Sanitizer 日志。它们用于追溯，不应被当作最新源码状态的唯一依据；最新状态以 `05_CURRENT_STATUS_AND_EVIDENCE.md`、`project_state.yaml` 和当前源码为准。


## Step24风门上电校准资料

- `archive/reports/DAMPER_BOOT_OPEN_STEP24_REPORT.md`
- `archive/test_logs/DAMPER_BOOT_OPEN_STEP24_TEST_LOG.txt`
- `reference/protocol/STM32_W2_step24_通讯协议_OpenCode生成解析规范.txt`


## Step25单总线多从机资料

- `archive/reports/ONEWIRE_MULTISLAVE_STEP25_REPORT.md`
- `archive/test_logs/ONEWIRE_MULTISLAVE_STEP25_TEST_LOG.txt`
- `reference/protocol/STM32_W2_step25_通讯协议_OpenCode生成解析规范.txt`

## Step26 学习版资料

```text
oc_v2/reference/learning/00_CODE_READING_GUIDE.md
oc_v2/reference/learning/01_ARCHITECTURE_AND_DATAFLOW.md
oc_v2/reference/learning/02_STATE_MACHINE_INDEX.md
oc_v2/reference/learning/03_DEBUGGING_AND_STUDY_GUIDE.md
oc_v2/reference/learning/04_GLOSSARY_AND_UNITS.md
```

Step26只增加注释和学习资料，不改变固件逻辑、协议字段或硬件配置。



## Step27 NTC Rcent查表资料

- `archive/reports/NTC_RCENT_LOOKUP_STEP27_REPORT.md`
- `archive/test_logs/NTC_RCENT_LOOKUP_STEP27_TEST_LOG.txt`
- `reference/protocol/STM32_W2_step27_通讯协议_OpenCode生成解析规范.txt`
- `reference/ntc/LNTD5.06_05_GW_RCENT_TABLE_SOURCE.md`

Step27改变NTC换算和测量有效范围，但不改变对象0x05的帧布局，也不改变AUTO的-25~60°C控制范围。
