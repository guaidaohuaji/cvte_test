# 手动目标转速控制 Phase 1 报告

## 1. 本阶段边界

本阶段只在单片机内部建立独立的“手动目标转速控制器”，尚未修改 W2 对象 `0x06`，也未修改上位机。

因此本阶段固件烧录后：

- 现有上位机仍只能执行关闭和 PWM 占空比控制；
- 现有 W2 帧格式完全不变；
- AUTO 风机与风门控制完全不变；
- 新控制器仅通过内部 C API 可调用，为下一阶段协议接入做准备。

## 2. 源码审计结论

### 2.1 启动加力

`AppFan` 已统一处理风机从关闭到启动的过程：

1. 首次使能时输出 100% PWM；
2. 保持 `APP_FAN_STARTUP_BOOST_MS = 5000 ms`；
3. 结束后切换到调用者请求的目标占空比；
4. 重新捕获测速。

因此手动目标转速控制不需要复制启动计时器，只需把查表前馈占空比交给 `AppFan_SetEnabled()`。

### 2.2 可复用控制参数

step15 已提供 `app_auto_fan_profile.h`，其中包含：

- 1000～2300 RPM 的分段线性插值；
- 10%～80% 正常占空比限制；
- 1%/2%/3% 分级调整步长；
- 进入 ±50 RPM、退出 ±80 RPM 的容差迟滞。

本阶段直接复用这些纯函数，避免 AUTO 与手动转速模式产生两套不同标定表。

### 2.3 控制所有权

新模块仅在 `AppAutoControl_GetMode() == APP_AUTO_MODE_MANUAL` 时运行。

进入全局 AUTO 后：

- 立即清除手动转速控制器内部模式和历史状态；
- 不向风机发送任何命令；
- 不恢复旧的手动目标；
- AUTO 继续独占风机与风门。

## 3. 新增内部接口

新增：

- `Core/Inc/app_manual_fan_control.h`
- `Core/Src/app_manual_fan_control.c`

主要 API：

```c
void AppManualFanControl_Init(void);
void AppManualFanControl_Process(void);
AppManualFanResult AppManualFanControl_SetOff(void);
AppManualFanResult AppManualFanControl_SetDuty(uint16_t duty_x100);
AppManualFanResult AppManualFanControl_SetTargetRpm(uint16_t target_rpm);
bool AppManualFanControl_GetSnapshot(AppManualFanControlSnapshot *snapshot);
```

内部控制模式：

- `OFF`
- `DUTY`
- `SPEED`

速度控制状态：

- 启动加力；
- 等待测速；
- 调节中；
- 已进入容差；
- 测速故障；
- 低端/高端饱和；
- 硬件错误。

## 4. 手动转速控制流程

调用 `AppManualFanControl_SetTargetRpm(target_rpm)` 后：

1. 检查当前必须为全局 MANUAL；
2. 检查目标范围为 1000～2300 RPM；
3. 使用 step15 标定表插值得到前馈 PWM；
4. 调用 `AppFan_SetEnabled(true, feedforward)`；
5. 风机原先关闭时，由 `AppFan` 自动执行 100%/5 秒启动加力；
6. 启动完成并获得有效测速后，每 1 秒闭环判断一次；
7. 大误差立即按 3% 修正；
8. 中小误差连续两次同方向后按 1% 或 2% 修正；
9. 进入 ±50 RPM 容差，超过 ±80 RPM 才重新调节；
10. 正常闭环限制在 10%～80%；
11. 持续无测速时切换到 100% 保护，测速恢复后重新回到前馈点。

## 5. 主循环接入

新增初始化：

```c
AppManualFanControl_Init();
```

新增周期处理，放在 `AppAutoControl_Process()` 之后：

```c
AppAutoControl_Process();
AppManualFanControl_Process();
```

这样 AUTO 在同一轮主循环中优先处理；手动控制器检测到 AUTO 后只清理自身状态，不会争抢输出。

## 6. 未修改内容

本阶段未修改：

- `app_uart.c`；
- W2 对象 `0x06` 控制帧；
- W2 对象 `0x06` 查询响应；
- 上位机；
- AUTO 算法；
- 风门控制；
- BPF 测速；
- 单总线；
- 其他 W2 对象。

## 7. 测试

新增测试覆盖：

- MANUAL/AUTO 所有权锁定；
- 目标转速范围校验；
- 1500 RPM 前馈插值为 23%；
- 5 秒启动加力期间不重复调整；
- 300 RPM 大误差立即调整 3%；
- 200 RPM 中误差两次确认后调整 2%；
- ±50/±80 RPM 容差迟滞；
- 无测速时进入 100% 保护且不重复发送；
- 测速恢复后重新回到前馈占空比；
- 切换 AUTO 后清除手动历史且不发送风机命令；
- Master/Slave 条件编译；
- AddressSanitizer/UndefinedBehaviorSanitizer。

同时重新执行 step15 AUTO 和 BPF 回归测试，均通过。

完整结果见 `../test_logs/MANUAL_FAN_PHASE1_TEST_LOG.txt`。

## 8. 构建说明

`build.ps1` 已加入 `app_manual_fan_control.c`。

当前环境未安装 ARM 交叉编译器，因此尚未生成新的 HEX/BIN，也未进行真实硬件验证。

## 9. 下一阶段

下一阶段应扩展 W2 对象 `0x06`：

- 保持旧命令 `mode=0/1` 完全兼容；
- 新增 `mode=2` 表示目标转速；
- 增加可选扩展查询，返回控制模式、目标 RPM、误差与控制阶段；
- 先完成固件协议和协议测试，不修改上位机。
