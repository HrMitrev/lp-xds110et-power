# Makefile for TI MSP432E401Y Firmware (XDS110 / LP-XDS110ET)
#
# Builds bare-metal firmware targeting flash offset 0x4000 (DFU application space)

TARGET     ?= firmware
BUILD_DIR   = build
PYTHON     ?= python

# Verbosity control (use V=1 or VERBOSE=1 for full command lines)
V          ?= 0
VERBOSE    ?= $(V)

ifeq ($(VERBOSE),1)
Q          :=
ECHO       := @:
else
Q          := @
ECHO       := @echo
endif

# Standalone toolchain path (if fetched locally)
TOOLCHAIN_DIR ?= toolchain

ifneq ($(wildcard $(TOOLCHAIN_DIR)/bin/arm-none-eabi-gcc.exe),)
TOOLCHAIN_BIN := $(abspath $(TOOLCHAIN_DIR)/bin)
export PATH   := $(TOOLCHAIN_BIN):$(PATH)
CC            := $(TOOLCHAIN_BIN)/arm-none-eabi-gcc
OBJCOPY       := $(TOOLCHAIN_BIN)/arm-none-eabi-objcopy
OBJDUMP       := $(TOOLCHAIN_BIN)/arm-none-eabi-objdump
SIZE          := $(TOOLCHAIN_BIN)/arm-none-eabi-size
else
CC            := arm-none-eabi-gcc
OBJCOPY       := arm-none-eabi-objcopy
OBJDUMP       := arm-none-eabi-objdump
SIZE          := arm-none-eabi-size
endif

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

# Linker options (links against newlib-nano with nosys syscall stubs)
LDFLAGS     = $(MCU_FLAGS) \
              --specs=nano.specs \
              --specs=nosys.specs \
              -T msp432e401y.ld \
              -nostartfiles \
              -Wl,--gc-sections \
              -Wl,-Map=$(BUILD_DIR)/$(TARGET).map

# Source files
APP_SRCS    = src/main.c src/startup_gcc.c src/board.c src/dfu_jump.c \
              src/usb/usb_descriptors.c src/usb/usb_device.c \
              src/protocol/xds_cmd.c \
              src/energytrace/et_hdr.c
DL_DIR      = sdk/ti/devices/msp432e4/driverlib
DL_SRCS     = $(wildcard $(DL_DIR)/sysctl.c $(DL_DIR)/gpio.c $(DL_DIR)/cpu.c \
                         $(DL_DIR)/interrupt.c $(DL_DIR)/fpu.c $(DL_DIR)/systick.c \
                         $(DL_DIR)/usb.c $(DL_DIR)/ssi.c $(DL_DIR)/udma.c)

FORMAT_FILES = $(wildcard src/*.c src/*.h src/*/*.c src/*/*.h)

SRCS        = $(APP_SRCS) $(DL_SRCS)
OBJS        = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean fetch-sdk sdk fetch-toolchain toolchain inspect flash clang-format compile-commands clangd-check

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex compile_commands.json

# Ensure SDK is fetched before compiling
$(OBJS): | $(BUILD_DIR) check-sdk

check-sdk:
	@if [ ! -f $(DL_DIR)/driverlib.h ]; then \
		echo "SDK not found. Fetching MSP432E4 DriverLib..."; \
		$(PYTHON) scripts/fetch_sdk.py; \
	fi

fetch-sdk sdk:
	$(Q)$(PYTHON) scripts/fetch_sdk.py

fetch-toolchain toolchain:
	$(Q)$(PYTHON) scripts/fetch_toolchain.py

$(BUILD_DIR):
	$(Q)mkdir -p $(BUILD_DIR)/src
	$(Q)mkdir -p $(BUILD_DIR)/sdk/ti/devices/msp432e4/driverlib

$(BUILD_DIR)/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) msp432e401y.ld
	$(ECHO) "  LD    $@"
	$(Q)$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(ECHO) "--- Size Information ---"
	$(Q)$(SIZE) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(ECHO) "  OBJCOPY $@"
	$(Q)$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(ECHO) "  OBJCOPY $@"
	$(Q)$(OBJCOPY) -O ihex $< $@

inspect: $(BUILD_DIR)/$(TARGET).bin
	$(Q)$(PYTHON) ../py_ref/firmware_analysis/inspect_firmware.py $<

flash: $(BUILD_DIR)/$(TARGET).bin
	$(Q)cargo run --release --manifest-path ../xds110-et/Cargo.toml -- dfu-flash -r $<

clang-format:
	$(ECHO) "  FMT   $(FORMAT_FILES)"
	$(Q)uvx clang-format@latest -i $(FORMAT_FILES)

compile_commands.json: Makefile scripts/gen_compile_commands.py
	$(ECHO) "  CDB   $@"
	$(Q)$(PYTHON) scripts/gen_compile_commands.py .

compile-commands:
	$(ECHO) "  CDB   compile_commands.json"
	$(Q)$(PYTHON) scripts/gen_compile_commands.py .

CLANGD ?= clangd

clangd-check cc-check: compile_commands.json
	$(Q)for f in $(FORMAT_FILES); do \
		echo "  CHECK $$f"; \
		$(CLANGD) --compile-commands-dir=. --tweaks= --log=error --check=$$f || exit 1; \
	done

clean:
	$(Q)rm -rf $(BUILD_DIR) compile_commands.json
