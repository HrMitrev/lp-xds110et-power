# Makefile for TI MSP432E401Y Firmware (XDS110 / LP-XDS110ET)
#
# Builds bare-metal firmware targeting flash offset 0x4000 (DFU application space)

TARGET     ?= firmware
BUILD_DIR   = build
PYTHON     ?= python

# Toolchain definitions
CC          = arm-none-eabi-gcc
OBJCOPY     = arm-none-eabi-objcopy
OBJDUMP     = arm-none-eabi-objdump
SIZE        = arm-none-eabi-size

# Architecture & Hardware Floating-Point flags for Cortex-M4F
MCU_FLAGS   = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16

# Include paths
INCLUDES    = -Isrc \
              -Isdk \
              -Isdk/ti/devices/msp432e4 \
              -Isdk/ti/devices/msp432e4/driverlib \
              -Isdk/ti/devices/msp432e4/inc \
              -Isdk/third_party/CMSIS/Include

# Preprocessor defines
DEFINES     = -Dgcc \
              -D__MSP432E401Y__ \
              -DTARGET_IS_MSP432E401Y \
              -DPART_MSP432E401Y

# Compiler options (size optimization, dead code stripping, standards compliance)
CFLAGS      = $(MCU_FLAGS) $(DEFINES) $(INCLUDES) \
              -Os -g3 -Wall -Wextra \
              -Wno-unused-parameter -Wno-old-style-declaration \
              -ffunction-sections -fdata-sections \
              -std=c99

# Linker options
LDFLAGS     = $(MCU_FLAGS) \
              -T msp432e401y.ld \
              -nostartfiles \
              -Wl,--gc-sections \
              -Wl,-Map=$(BUILD_DIR)/$(TARGET).map

# Source files
APP_SRCS    = src/main.c src/startup_gcc.c
DL_DIR      = sdk/ti/devices/msp432e4/driverlib
DL_SRCS     = $(wildcard $(DL_DIR)/sysctl.c $(DL_DIR)/gpio.c $(DL_DIR)/cpu.c $(DL_DIR)/interrupt.c)

SRCS        = $(APP_SRCS) $(DL_SRCS)
OBJS        = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean fetch-sdk sdk inspect flash

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex

# Ensure SDK is fetched before compiling
$(OBJS): | $(BUILD_DIR) check-sdk

check-sdk:
	@if [ ! -f $(DL_DIR)/driverlib.h ]; then \
		echo "SDK not found. Fetching MSP432E4 DriverLib..."; \
		$(PYTHON) scripts/fetch_sdk.py; \
	fi

fetch-sdk sdk:
	$(PYTHON) scripts/fetch_sdk.py

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)/src
	@mkdir -p $(BUILD_DIR)/sdk/driverlib

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) msp432e401y.ld
	@echo "  LD    $@"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "--- Size Information ---"
	@$(SIZE) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	@echo "  OBJCOPY $@"
	@$(OBJCOPY) -O ihex $< $@

inspect: $(BUILD_DIR)/$(TARGET).bin
	$(PYTHON) ../py_ref/firmware_analysis/inspect_firmware.py $<

flash: $(BUILD_DIR)/$(TARGET).bin
	cargo run --release --manifest-path ../xds110-et/Cargo.toml -- dfu-flash -f $< -r

clean:
	rm -rf $(BUILD_DIR)
