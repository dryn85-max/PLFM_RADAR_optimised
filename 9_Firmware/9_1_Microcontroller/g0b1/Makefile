# STM32G0B1 (NUCLEO-G0B1RE) firmware. Targets: all, size-check, flash, test, clean.
ADAR_COUNT ?= 1
DIAG       ?= 1
PROGRAMMER ?= stlink

TARGET := aeris_g0b1
BUILD  := build
CC     := arm-none-eabi-gcc
OBJCOPY:= arm-none-eabi-objcopy
SIZE   := arm-none-eabi-size

HAL_DIR  := Drivers/STM32G0xx_HAL_Driver
CMSIS_DEV:= Drivers/CMSIS/Device/ST/STM32G0xx/Include
CMSIS_INC:= Drivers/CMSIS/Include

APP_SRCS := $(wildcard Core/*.c) $(wildcard Core/hal/*.c) \
            $(wildcard Core/drivers/*.c) $(wildcard Core/app/*.c)
HAL_SRCS := $(wildcard $(HAL_DIR)/Src/*.c)
ASM_SRCS := startup_stm32g0b1xx.s

DEFS := -DSTM32G0B1xx -DUSE_HAL_DRIVER -DADAR_COUNT=$(ADAR_COUNT)
ifeq ($(DIAG),0)
DEFS += -DDIAG_DISABLE
endif

INCS := -ICore -ICore/hal -ICore/drivers -ICore/app \
        -I$(HAL_DIR)/Inc -I$(HAL_DIR)/Inc/Legacy -I$(CMSIS_DEV) -I$(CMSIS_INC)

ARCH   := -mcpu=cortex-m0plus -mthumb
CFLAGS := $(ARCH) -Os -std=c11 -Wall -Wextra -Werror \
          -ffunction-sections -fdata-sections --specs=nano.specs $(DEFS) $(INCS)
# Vendored sources that need relaxed warnings (none so far; add -Wno-unused-parameter here if one fails -Wextra).
VENDOR_CFLAGS :=
LDFLAGS := $(ARCH) --specs=nano.specs -TSTM32G0B1RETx_FLASH.ld \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -lc -lm -lnosys

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(APP_SRCS) $(HAL_SRCS)) $(patsubst %.s,$(BUILD)/%.o,$(ASM_SRCS))

.PHONY: all size-check flash test clean
all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex
	$(SIZE) $(BUILD)/$(TARGET).elf
	@$(MAKE) --no-print-directory size-check

$(BUILD)/$(TARGET).elf: $(OBJS) STM32G0B1RETx_FLASH.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@
$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(HAL_DIR)/Src/%.o: $(HAL_DIR)/Src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(VENDOR_CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

-include $(OBJS:.o=.d)

# flash <= 128 KB (text+rodata+data), RAM <= 32 KB (data+bss+noinit+stack+heap reserve)
MIN_STACK := 0x1000
MIN_HEAP  := 0x400
size-check: $(BUILD)/$(TARGET).elf
	@$(SIZE) -A $< | awk -v stack=$$(($(MIN_STACK))) -v heap=$$(($(MIN_HEAP))) ' \
	  $$1 ~ /^\.(isr_vector|text|rodata|data|init_array|fini_array|preinit_array|ARM|ARM\.extab)$$/ {flash+=$$2} \
	  $$1 ~ /^\.(data|bss|noinit)$$/ {ram+=$$2} \
	  END { ram += stack + heap; \
	    printf "size-check: flash %d / 131072, ram %d / 32768\n", flash, ram; \
	    if (flash > 131072 || ram > 32768) { print "size-check FAILED"; exit 1 } }'

flash: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
ifeq ($(PROGRAMMER),cube)
	STM32_Programmer_CLI -c port=SWD -w $(BUILD)/$(TARGET).elf -rst
else
	st-flash --reset write $(BUILD)/$(TARGET).bin 0x08000000
endif

test:
	$(MAKE) -C tests ADAR_COUNT=1
	$(MAKE) -C tests ADAR_COUNT=4

clean:
	rm -rf $(BUILD)
	$(MAKE) -C tests clean
