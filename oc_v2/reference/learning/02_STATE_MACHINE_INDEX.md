# Step26 状态机索引

## PWM输入

```text
SEARCH
├─ 收到有效DMA pair → OK
├─ 超时且引脚高 → STATIC_HIGH
├─ 超时且引脚低 → STATIC_LOW
├─ 频率超上限 → OUT_OF_RANGE
└─ DMA错误 → HW_ERROR
```

量程独立于业务状态：FAST ↔ MEDIUM ↔ SLOW。

## 风机底层

```text
OFF
→ STARTUP_BOOST（5秒100%）
→ RUNNING
├─ NO_TACH
├─ TACH_UNRELIABLE
├─ PWM_ERROR
└─ SAFETY_LOCKED
```

## 手动/AUTO风机控制

```text
INACTIVE → STARTING → WAIT_TACH → ADJUSTING ↔ IN_TOLERANCE
                                  ├→ SATURATED_LOW/HIGH
                                  ├→ TACH_FAULT
                                  ├→ HW_ERROR
                                  └→ SAFETY_LOCKED
```

## 风机健康

```text
DISABLED/FAN_OFF
→ SETTLING
→ MONITORING
→ SUSPECT_LOW / SUSPECT_HIGH / SUSPECT_NO_TACH
→ 维持5秒
→ LATCHED_LOW / LATCHED_HIGH / LATCHED_NO_TACH
```

偏差进入门限 600 RPM，恢复门限 450 RPM。

## 风门

```text
BOOT_HOMING（上电1700步打开）
→ POST_MOVE_HOLD
→ IDLE_RELEASED
→ MOVING_FORWARD / MOVING_REVERSE
→ POST_MOVE_HOLD
→ IDLE_RELEASED
```

STOP/RELEASE/紧急停机可中止；校准中止后位置无效。

## AUTO总状态

```text
UNINITIALIZED
→ MANUAL
或
→ WAIT_TEMP → TARGET_READY
             └→ TEMP_FAULT
```

风机和风门还有各自子状态，不应只看 AUTO 总状态判断执行器是否已经到位。

## 单总线Master

```text
IDLE
→ HANDSHAKE_START
→ HS1_TX → HS1_WAIT
→ GUARD
→ HS2_TX → HS2_WAIT
→ GUARD
→ READ/WRITE_TX → WAIT
→ IDLE
```

若上下文仍 ONLINE 且响应年龄小于 450 ms，READ/WRITE 可从 IDLE 直接进入操作阶段。

## 单总线Slave

```text
WAIT_HANDSHAKE_1
→ WAIT_HANDSHAKE_2
→ ONLINE
→ 超过500ms无本机有效帧
→ COMM_FAULT
→ 新HS1重新开始
```
