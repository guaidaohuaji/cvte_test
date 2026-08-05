> **Step27提示**：本工程保留Step26详细注释与学习资料，并将NTC改为LTR `LNTD5.06(05)GW` 的 `Rcent` 查表换算。
> NTC测量范围为 `-40~120°C`；AUTO控制范围仍为 `-25~60°C`。
> 推荐阅读入口仍是 `oc_v2/reference/learning/00_CODE_READING_GUIDE.md`。

# STM32F407 完整工程：OpenCode 交接入口

交接资料版本：V3 + Step27 NTC Rcent查表与量程扩展
更新日期：2026-08-03

V3 在 V2 基础上新增:
- TB6612 风门两相步进电机驱动 (对象 0x07)
- TIM6 步进节拍定时器
- Error_Handler 故障安全接入
- 5 路致命异常处理器风门紧急关闭
- 完整开发 Step 1~5.1 已完成

## 1. 当前本地工程路径

```text
C:\Users\user\Desktop\document\project\cvte_test-step11-led-arbitration\cvte_test-step11-led-arbitration
```

建议把本目录放在工程根目录下：

```text
C:\Users\user\Desktop\document\project\cvte_test-step11-led-arbitration\cvte_test-step11-led-arbitration\oc_v2
```

## 2. 本次更新内容

V1 主要描述单总线功能。V2 已补充读取当前第11步工程源码得到的既有应用功能：

- 通用 PWM 输出；
- PWM 输入检测；
- 风机 PWM 控制；
- 风机反馈频率和转速检测；
- ADC1 双通道 DMA 扫描；
- NTC 温度检测；
- PE8 LED；
- PD13 数字输出；
- USART1/W2 对象 `0x01~0x08`；
- 相关定时器、ADC、DMA、GPIO、中断和数据流；
- 既有功能的源码级风险与未验证项。

本次源码提取依据：

```text
cvte_test-step11-led-arbitration.zip
SHA256: 65e30ab405b10481fa6030d2bebcdcac924979193547c477b8a4b96d64c675ae
```

用户已确认本地工程与该源码副本相同。OpenCode 仍应以本地真实源码为最终依据。

## 3. OpenCode 阅读顺序

1. `00_READ_ME_FIRST.md`
2. `01_PROJECT_OVERVIEW.md`
3. `02_HARDWARE_WIRING.md`
4. `03_SOFTWARE_ARCHITECTURE.md`
5. `04_PROTOCOL_AND_COMMANDS.md`
6. `05_CURRENT_STATUS_AND_EVIDENCE.md`
7. `06_BUILD_FLASH_AND_TEST.md`
8. `07_KNOWN_RISKS_AND_BACKLOG.md`
9. `08_OPENCODE_WORKFLOW.md`
10. `09_SOURCE_INDEX.md`
11. `10_EXISTING_APPLICATION_FEATURES.md`
12. `11_EXISTING_FEATURE_TEST_COMMANDS.md`
13. `12_V2_SOURCE_EXTRACTION_NOTES.md`
14. `project_state.yaml`
15. `serial_test_vectors.txt`
16. `根目录中的 ref_src.txt、ref_audit.txt、ref_tests.txt`中的审计和提取报告

随后必须读取本地工程的真实源码，并核对交接资料。

## 4. 第一次对话规则

第一次只做只读审计：

- 不修改代码；
- 不构建；
- 不下载；
- 不运行 STM32CubeMX；
- 报告实际文件路径、函数名、宏名和差异；
- 区分已验证行为、源码事实、静态风险和推断；
- 完成后停止，等待用户提出下一步需求。

## 5. 当前最重要结论

双板单总线核心链路已经硬件验证通过：

```text
写 0x0008 = 0x1234
→ 等待链路过期
→ 读命令触发自动重新握手
→ 读回 0x1234
```

风机、PWM输入、NTC等既有模块已经在源码中实现，但本交接对话没有保存它们的完整硬件验证记录。因此只能标记为“源码存在”，不能自动标记为“硬件通过”。

## 6. 绝对禁止的误解

- `link_state = STALE`不等于最近事务失败；
- USART1 A2 返回 `STATUS_OK`只表示命令提交成功；
- 静态分析风险不等于硬件已经发生故障；
- 风机、NTC、PWM输入存在源码不等于已完成本轮硬件验证；
- 不得因为新增需求破坏 USART1 对象 `0x01~0x06`；
- 不得擅自恢复单总线握手后自动循环读写。


## 文档归档

历史阶段报告、测试日志、协议详细规范和滤波器设计资料已统一移动到 `oc_v2/` 子目录。目录说明见 `13_DOCUMENT_ARCHIVE_INDEX.md`。工程根目录不再存放这些 `.md`/`.txt`/日志文件。


## Step23 最新增量

- Step21通用PWM输出对象0x01已完成Phase P1，并由用户确认硬件测试正常；
- Step22通用PWM输入1～20 kHz硬件基线已由用户确认；
- Step23默认改为TIM1 PWM Input + DMA2 Stream6 Channel0循环采集，正式软件范围扩展到1～200 kHz；
- 采用FAST/MEDIUM/SLOW三档PSC自动量程，主循环读取DMA最新样本并取最近5周期中位数；
- 对象0x03的W2布局和用户确认的占空比语义均未改变；
- `APP_PWM_INPUT_ENGINE_MODE`可一行回退到Step22双边沿中断引擎。


## Step24 最新增量

- Master构建上电后自动沿打开方向强制移动风门1700步；
- 当前实物方向为正向关闭、反向打开，因此启动校准使用反向相序；
- 校准期间对象0x07返回`DAMPER_STATE_BOOT_HOMING(0x08)`，位置无效并拒绝普通位置命令；
- 校准完成后把机械全开端建立为`0步/90°`，保持100ms后释放；
- STOP、RELEASE或紧急停机中断校准时，不建立位置参考；
- 对象0x07 DATA长度不变，flags bit5用于标记启动校准活动。
