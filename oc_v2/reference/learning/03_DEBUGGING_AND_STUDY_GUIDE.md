# Step26 调试与单步阅读指南

## 1. 推荐断点

### W2命令

```text
AppUart_RxCpltCallback
handle_byte
process_a2_control
目标模块公开API
```

不要长时间停在 RX 中断中，否则会人为制造串口丢字节。

### 风机

```text
AppManualFanControl_SetTargetRpm
fan_control_process / AppManualFanControl_Process
AppFan_SetEnabled
apply_duty
AppFanHealth_Process
latch_fault
```

### 风门

```text
AppDamper_Init
damper_start_boot_homing
damper_start_motion
AppDamper_TimerCallback
AppDamper_Process
```

TIM6 回调每秒 300 次，条件断点优先，例如 `remaining_steps < 3`。

### 单总线

Master：

```text
AppOneWireMaster_SubmitTo
begin_handshake
process_complete_frame
finish_success/fail_*
```

Slave：

```text
frame_is_for_slave
process_frame
handle_handshake_2
handle_read/handle_write
process_state_timeouts
```

## 2. 推荐观察变量

```text
app_uart: parser_state, frame_idx, frame_len, rx_head, rx_tail
app_pwm: current_psc, current_arr, current_ccr, current_mode
app_pwm_input: active_profile, valid_pair_count, dma_sample_count
app_fan: state, target_duty_x100, applied_duty_x100
app_fan_health: health_snapshot, suspect_start_tick
app_damper: damper_state, remaining_steps, current_steps, boot_homing_active
onewire master: master_state, busy_flag, active_context_index, pending_*
onewire slave: slave_state, last_valid_request_tick, response_pending
```

静态文件变量在优化等级 O0 下最容易观察；当前 build.ps1 已使用 `-O0 -g3`。

## 3. 阅读和修改规则

1. 修改门限先改 config.h，不要在 .c 中新增第二份魔数。
2. 新增状态后同步更新：枚举、W2映射、上位机映射、测试和协议文档。
3. 新增 .c 后同步加入 build.ps1 的 `$C_FILES`。
4. HAL weak callback 只能有一个强定义，新增回调要在现有分发点扩展。
5. 中断和主循环共享变量要考虑 `volatile`、原子宽度和读写顺序。
6. 不要在 `Process()` 中加入 `HAL_Delay()`。
7. 修改前先为目标行为补测试，修改后跑对应脚本和全量回归。
