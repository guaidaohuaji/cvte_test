$ErrorActionPreference = "Stop"

$TOOLCHAIN = "$env:LOCALAPPDATA\arm-gnu-toolchain\bin"
$CC        = "$TOOLCHAIN\arm-none-eabi-gcc.exe"
$OBJCOPY   = "$TOOLCHAIN\arm-none-eabi-objcopy.exe"
$SZ        = "$TOOLCHAIN\arm-none-eabi-size.exe"

$CPU   = "-mcpu=cortex-m4"
$FPU   = "-mfpu=fpv4-sp-d16"
$FLOAT = "-mfloat-abi=hard"
$MCU   = "$CPU -mthumb $FPU $FLOAT"

$C_DEFS = "-DUSE_HAL_DRIVER -DSTM32F407xx"

$C_INCLUDES = @(
    "-ICore/Inc",
    "-IDrivers/STM32F4xx_HAL_Driver/Inc",
    "-IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy",
    "-IDrivers/CMSIS/Device/ST/STM32F4xx/Include",
    "-IDrivers/CMSIS/Include"
)

$CFLAGS = "$MCU $C_DEFS $($C_INCLUDES -join ' ') -Wall -Wextra -fdata-sections -ffunction-sections -O0 -g3 -std=gnu11 -MMD -MP"
$ASFLAGS = "$MCU -Wall -fdata-sections -ffunction-sections"

$LDFLAGS = "$MCU -specs=nosys.specs -TSTM32F407VETx_FLASH.ld -Wl,-Map=build/test.map,--cref -Wl,--gc-sections -lm"

$BUILD_DIR = "build"
$TARGET = "test"

if (-not (Test-Path $BUILD_DIR)) {
    New-Item -ItemType Directory -Force -Path $BUILD_DIR | Out-Null
}

$ASM_FILES = @("Core/Startup/startup_stm32f407xx.s")

$C_FILES = @(
    "Core/Src/main.c",
    "Core/Src/gpio.c",
    "Core/Src/usart.c",
    "Core/Src/dma.c",
    "Core/Src/app_uart.c",
    "Core/Src/app_pwm.c",
    "Core/Src/app_pwm_input.c",
    "Core/Src/adc.c",
    "Core/Src/app_ntc.c",
    "Core/Src/app_fan.c",
    "Core/Src/app_adc_scan.c",
    "Core/Src/app_fan_feedback_adc.c",
    "Core/Src/stm32f4xx_it.c",
    "Core/Src/tim.c",
    "Core/Src/stm32f4xx_hal_msp.c",
    "Core/Src/system_stm32f4xx.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c",
    "Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c"
)

$objects = @()

function Build-Object($src, $flags) {
    $obj = "$BUILD_DIR\$([System.IO.Path]::GetFileNameWithoutExtension($src)).o"
    Write-Host "CC $($src)"
    $cmd = "`"$CC`" -c $flags `"$src`" -o `"$obj`""
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) {
        throw "Compile failed: $src"
    }
    return $obj
}

Write-Host "===== Building $TARGET ====="
Write-Host ""

foreach ($src in $ASM_FILES) {
    $objects += (Build-Object $src $ASFLAGS)
}
foreach ($src in $C_FILES) {
    $objects += (Build-Object $src $CFLAGS)
}

Write-Host "LD  $BUILD_DIR\$TARGET.elf"
$objList = ($objects -join '" "')
$ldCmd = "`"$CC`" `"$objList`" $LDFLAGS -o `"$BUILD_DIR\$TARGET.elf`""
cmd /c $ldCmd
if ($LASTEXITCODE -ne 0) { throw "Link failed" }

Write-Host "HEX $BUILD_DIR\$TARGET.hex"
& $OBJCOPY -O ihex "$BUILD_DIR\$TARGET.elf" "$BUILD_DIR\$TARGET.hex"
if ($LASTEXITCODE -ne 0) { throw "HEX generation failed" }

Write-Host "BIN $BUILD_DIR\$TARGET.bin"
& $OBJCOPY -O binary "$BUILD_DIR\$TARGET.elf" "$BUILD_DIR\$TARGET.bin"
if ($LASTEXITCODE -ne 0) { throw "BIN generation failed" }

Write-Host ""
Write-Host "===== Build succeeded ====="
& $SZ "$BUILD_DIR\$TARGET.elf"
