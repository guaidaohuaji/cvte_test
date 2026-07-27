# 完整源码阅读清单

## 1. 顶层和生成配置

```text
test.ioc
build.ps1
STM32F407VETx_FLASH.ld
.vscode/tasks.json
.vscode/launch.json
.vscode/c_cpp_properties.json
```

## 2. 应用头文件

```text
Core/Inc/app_uart.h
Core/Inc/app_led.h

Core/Inc/app_pwm.h
Core/Inc/app_pwm_input.h

Core/Inc/app_adc_scan.h
Core/Inc/app_ntc.h
Core/Inc/app_ntc_config.h
Core/Inc/app_fan.h
Core/Inc/app_fan_config.h
Core/Inc/app_fan_feedback_adc.h
Core/Inc/app_fan_feedback_bpf.h
Core/Inc/app_auto_fan_profile.h
Core/Inc/app_auto_control.h
Core/Inc/app_auto_control_config.h
Core/Inc/app_manual_fan_control.h

Core/Inc/app_onewire.h
Core/Inc/app_onewire_config.h
Core/Inc/app_onewire_protocol.h
Core/Inc/app_onewire_uart.h
Core/Inc/app_onewire_master.h
Core/Inc/app_onewire_slave.h

Core/Inc/app_damper.h
Core/Inc/app_damper_config.h
```

## 3. 应用源码

```text
Core/Src/main.c
Core/Src/app_uart.c
Core/Src/app_led.c

Core/Src/app_pwm.c
Core/Src/app_pwm_input.c

Core/Src/app_adc_scan.c
Core/Src/app_ntc.c
Core/Src/app_fan.c
Core/Src/app_fan_feedback_adc.c
Core/Src/app_fan_feedback_bpf.c
Core/Src/app_auto_control.c
Core/Src/app_manual_fan_control.c

Core/Src/app_onewire.c
Core/Src/app_onewire_protocol.c
Core/Src/app_onewire_uart.c
Core/Src/app_onewire_master.c
Core/Src/app_onewire_slave.c

Core/Src/app_damper.c
```

## 4. 外设生成代码

```text
Core/Src/gpio.c
Core/Src/tim.c
Core/Src/adc.c
Core/Src/dma.c
Core/Src/usart.c
Core/Src/stm32f4xx_it.c
Core/Src/stm32f4xx_hal_msp.c

Core/Inc/main.h
Core/Inc/gpio.h
Core/Inc/tim.h
Core/Inc/adc.h
Core/Inc/dma.h
Core/Inc/usart.h
Core/Inc/stm32f4xx_it.h
```

## 5. 测试

```text
Tests/test_app_onewire_protocol.c
Tests/test_app_onewire_uart.c
Tests/test_app_onewire_master.c
Tests/test_app_onewire_slave.c
Tests/test_app_onewire_role.c
Tests/test_app_uart_onewire.c
Tests/test_app_uart_fan_manual.c
Tests/test_app_led.c
Tests/test_app_fan_startup.c
Tests/test_app_auto_fan_profile.c
Tests/test_app_auto_control_fan.c
Tests/test_app_manual_fan_control.c
Tests/test_app_fan_feedback_bpf.c
Tests/test_app_fan_feedback_reset.c
Tests/test_app_fan_feedback_source.c
Tests/test_app_fan_feedback_integration.c
```

关键回归脚本：

```text
Tests/run_manual_fan_phase2_host_test.sh
Tests/run_manual_fan_phase1_host_test.sh
Tests/run_fan_auto_host_test.sh
Tests/run_fan_bpf_host_test.sh
```

## 6. 全工程搜索词

```text
OBJ_PWM_OUT
OBJ_LED
OBJ_PWM_IN
OBJ_PD13
OBJ_NTC
OBJ_FAN
OBJ_ONEWIRE

HAL_ADC_Start_DMA
HAL_ADC_ConvHalfCpltCallback
HAL_ADC_ConvCpltCallback
dma_seq
half_ready
full_ready

AppPwm_
AppPwmInput_
AppNtc_
AppFan_
AppFanFeedback_
AppManualFanControl_
AppAutoControl_
AppAutoFan_
AppAdcScan_

TIM1
TIM2
TIM4
TIM10
ADC1
DMA2_Stream0

INPUT_INVERTED
raw_high_ticks
last_fall_ts
APP_FAN_RPM_FACTOR
APP_NTC_SAMPLE_COUNT

HAL_UART_RxCpltCallback
HAL_UART_TxCpltCallback
HAL_UART_ErrorCallback
HAL_TIM_IC_CaptureCallback
HAL_TIM_PeriodElapsedCallback
```

## 7. 必查赋值

- 所有状态枚举赋值；
- 所有`enabled`、`target_duty`、`applied_duty`；
- 所有`dma_seq`、`half_ready`、`full_ready`；
- 所有`update_seq`；
- 所有HAL回调定义；
- 所有`HAL_*_Start*`；
- 所有引脚初始化；
- `build.ps1`中的全部源文件。
