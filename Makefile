PROJECT = test
TARGET  = $(PROJECT)

TOOLCHAIN = $(LOCALAPPDATA)\arm-gnu-toolchain\bin
CC      = $(TOOLCHAIN)\arm-none-eabi-gcc.exe
OBJCOPY = $(TOOLCHAIN)\arm-none-eabi-objcopy.exe
SZ      = $(TOOLCHAIN)\arm-none-eabi-size.exe

CPU     = -mcpu=cortex-m4
FPU     = -mfpu=fpv4-sp-d16
FLOAT   = -mfloat-abi=hard
MCU     = $(CPU) -mthumb $(FPU) $(FLOAT)

AS_DEFS =
C_DEFS  = -DUSE_HAL_DRIVER -DSTM32F407xx

AS_INCLUDES =
C_INCLUDES = -ICore/Inc
C_INCLUDES += -IDrivers/STM32F4xx_HAL_Driver/Inc
C_INCLUDES += -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy
C_INCLUDES += -IDrivers/CMSIS/Device/ST/STM32F4xx/Include
C_INCLUDES += -IDrivers/CMSIS/Include

ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) -Wall -fdata-sections -ffunction-sections
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) -Wall -Wextra -fdata-sections -ffunction-sections
CFLAGS += -O0 -g3 -std=gnu11

ifeq ($(DEBUG), 1)
CFLAGS += -O0 -g3
else
CFLAGS += -Og -g3
endif

LDFLAGS  = $(MCU) -specs=nosys.specs -TSTM32F407VETx_FLASH.ld
LDFLAGS += -Wl,-Map=build/$(TARGET).map,--cref -Wl,--gc-sections

AS_SOURCES  = Core/Startup/startup_stm32f407xx.s

C_SOURCES   = Core/Src/main.c
C_SOURCES  += Core/Src/gpio.c
C_SOURCES  += Core/Src/stm32f4xx_it.c
C_SOURCES  += Core/Src/stm32f4xx_hal_msp.c
C_SOURCES  += Core/Src/system_stm32f4xx.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
C_SOURCES  += Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c

OBJECTS = $(addprefix build/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix build/,$(notdir $(AS_SOURCES:.s=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.s $(sort $(dir $(AS_SOURCES)))

.PHONY: all clean flash

all: build/$(TARGET).elf build/$(TARGET).hex build/$(TARGET).bin
	@$(SZ) build/$(TARGET).elf

build/%.o: %.c Makefile | build
	@echo "CC    $<"
	@$(CC) -c $(CFLAGS) -MMD -MP $< -o $@

build/%.o: %.s Makefile | build
	@echo "AS    $<"
	@$(CC) -c $(ASFLAGS) $< -o $@

build/$(TARGET).elf: $(OBJECTS) Makefile
	@echo "LD    $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) -o $@

build/%.hex: build/%.elf | build
	@$(OBJCOPY) -O ihex $< $@

build/%.bin: build/%.elf | build
	@$(OBJCOPY) -O binary $< $@

build:
	@if not exist build mkdir build

clean:
	@if exist build rmdir /s /q build

-include $(wildcard build/*.d)
