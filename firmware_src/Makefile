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

# Black Magic Debug Probe (BMP) build options
#   BMP_ENABLE=1       Compile & link upstream Black Magic Debug GDB server (GPLv3)
#   BMP_DEFAULT_MODE=0 Boot in Normal mode (0) or BMP mode on CDC1 (1)
BMP_ENABLE       ?= 1
BMP_DEFAULT_MODE ?= 0

# Preprocessor defines
DEFINES     = -Dgcc \
              -D__MSP432E401Y__ \
              -DTARGET_IS_MSP432E401Y \
              -DPART_MSP432E401Y \
              -DFW_DEFAULT_BMP_MODE=$(BMP_DEFAULT_MODE) \
              -DFW_BMP_GDB_COMPILED=$(BMP_ENABLE)

# Compiler options (size optimization, dead code stripping, standards compliance)
CFLAGS      = $(MCU_FLAGS) $(DEFINES) $(INCLUDES) \
              -Os -g3 -Wall -Wextra \
              -Wno-unused-parameter -Wno-old-style-declaration \
              -ffunction-sections -fdata-sections \
              -MMD -MP \
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
              src/energytrace/et_hdr.c \
              src/uart/cdc_uart.c \
              src/gpio/aux_gpio.c \
              src/dap/cmsis_dap.c src/dap/swd.c src/dap/xds_swd.c
DL_DIR      = sdk/ti/devices/msp432e4/driverlib
DL_SRCS     = $(DL_DIR)/sysctl.c $(DL_DIR)/gpio.c $(DL_DIR)/cpu.c \
              $(DL_DIR)/interrupt.c $(DL_DIR)/fpu.c $(DL_DIR)/systick.c \
              $(DL_DIR)/usb.c $(DL_DIR)/ssi.c $(DL_DIR)/udma.c \
              $(DL_DIR)/uart.c
SDK_STAMP   = $(DL_DIR)/driverlib.h

# Black Magic Debug core & Cortex-M targets (compiled as gnu11 when BMP_ENABLE=1)
BMP_DIR       = third_party/blackmagic/src
BMP_STAMP     = $(BMP_DIR)/gdb_main.c
BMP_VERSION_H = $(BUILD_DIR)/bmp/version.h

ifeq ($(BMP_ENABLE),1)
BMP_INCLUDES  = -Isrc/bmp -I$(BUILD_DIR)/bmp -I$(BMP_DIR) -I$(BMP_DIR)/include -I$(BMP_DIR)/target
BMP_DEFINES   = -DNO_LIBOPENCM3=1 -DCONFIG_BMDA=0 -DENABLE_DEBUG=0 -DENABLE_RTT=1 \
                -DADVERTISE_NOACKMODE=1 \
                -DCONFIG_CORTEXM=1 -DCONFIG_STM=1 -DCONFIG_NRF=1 -DCONFIG_TI=1 \
                -DCONFIG_TI_ICEPICK=1 -DCONFIG_LPC=1 -DCONFIG_SAM=1 -DCONFIG_NXP=1 \
                -DCONFIG_RA=1 -DCONFIG_PUYA=1 -DCONFIG_GD32=1
BMP_CFLAGS    = $(MCU_FLAGS) $(DEFINES) $(BMP_DEFINES) $(BMP_INCLUDES) $(INCLUDES) \
                -Os -g3 -Wall \
                -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
                -ffunction-sections -fdata-sections \
                -MMD -MP \
                -std=gnu11
BMP_BRIDGE_SRCS = src/bmp/bmp_mode.c src/bmp/bmp_platform.c src/bmp/bmp_tap.c
BMP_CORE_SRCS = $(BMP_DIR)/command.c $(BMP_DIR)/crc32.c $(BMP_DIR)/exception.c \
                $(BMP_DIR)/gdb_main.c $(BMP_DIR)/gdb_packet.c $(BMP_DIR)/hex_utils.c \
                $(BMP_DIR)/maths_utils.c $(BMP_DIR)/morse.c $(BMP_DIR)/remote.c \
                $(BMP_DIR)/rtt.c $(BMP_DIR)/timing.c
BMP_TARGET_SRCS = $(BMP_DIR)/target/adi.c $(BMP_DIR)/target/adiv5.c \
                  $(BMP_DIR)/target/adiv5_jtag.c $(BMP_DIR)/target/adiv5_swd.c \
                  $(BMP_DIR)/target/adiv6.c $(BMP_DIR)/target/cortex.c \
                  $(BMP_DIR)/target/cortexm.c $(BMP_DIR)/target/gdb_reg.c \
                  $(BMP_DIR)/target/jtag_devs.c $(BMP_DIR)/target/jtag_scan.c \
                  $(BMP_DIR)/target/onboard_flash.c $(BMP_DIR)/target/semihosting.c \
                  $(BMP_DIR)/target/sfdp.c $(BMP_DIR)/target/spi.c \
                  $(BMP_DIR)/target/target.c $(BMP_DIR)/target/target_flash.c \
                  $(BMP_DIR)/target/target_probe.c \
                  $(BMP_DIR)/target/stm32_common.c $(BMP_DIR)/target/stm32f1.c \
                  $(BMP_DIR)/target/stm32f4.c $(BMP_DIR)/target/stm32g0.c \
                  $(BMP_DIR)/target/stm32h5.c $(BMP_DIR)/target/stm32h7.c \
                  $(BMP_DIR)/target/stm32l0.c $(BMP_DIR)/target/stm32l4.c \
                  $(BMP_DIR)/target/stm32mp15.c $(BMP_DIR)/target/stm32wb0.c \
                  $(BMP_DIR)/target/nrf51.c $(BMP_DIR)/target/nrf54l.c $(BMP_DIR)/target/nrf91.c \
                  $(BMP_DIR)/target/msp432e4.c $(BMP_DIR)/target/msp432p4.c \
                  $(BMP_DIR)/target/ti_cc_common.c $(BMP_DIR)/target/cc23xx.c \
                  $(BMP_DIR)/target/mspm0.c $(BMP_DIR)/target/icepick.c \
                  $(BMP_DIR)/target/lpc_common.c $(BMP_DIR)/target/lpc11xx.c \
                  $(BMP_DIR)/target/lpc15xx.c $(BMP_DIR)/target/lpc17xx.c \
                  $(BMP_DIR)/target/lpc40xx.c $(BMP_DIR)/target/lpc43xx.c \
                  $(BMP_DIR)/target/lpc546xx.c $(BMP_DIR)/target/lpc55xx.c \
                  $(BMP_DIR)/target/sam3x.c $(BMP_DIR)/target/sam4l.c \
                  $(BMP_DIR)/target/samd.c $(BMP_DIR)/target/samx5x.c \
                  $(BMP_DIR)/target/imxrt.c $(BMP_DIR)/target/kinetis.c \
                  $(BMP_DIR)/target/nxpke04.c $(BMP_DIR)/target/s32k3xx.c \
                  $(BMP_DIR)/target/renesas_ra.c $(BMP_DIR)/target/puya.c
BMP_SRCS      = $(BMP_BRIDGE_SRCS) $(BMP_CORE_SRCS) $(BMP_TARGET_SRCS)
BMP_OBJS      = $(patsubst %.c,$(BUILD_DIR)/%.o,$(BMP_SRCS))
else
APP_SRCS     += src/bmp/bmp_mode.c
BMP_SRCS      =
BMP_OBJS      =
endif

FORMAT_FILES = $(wildcard src/*.c src/*.h src/*/*.c src/*/*.h)

SRCS        = $(APP_SRCS) $(DL_SRCS)
APP_OBJS    = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS        = $(APP_OBJS) $(BMP_OBJS)
DEPS        = $(OBJS:.o=.d)

.PHONY: all clean check-sdk fetch-sdk sdk fetch-bmp bmp fetch-toolchain toolchain inspect flash clang-format compile-commands clangd-check test-hil test-swd

all: $(BUILD_DIR)/$(TARGET).bin $(BUILD_DIR)/$(TARGET).hex compile_commands.json

# Ensure SDK (and BMP sources when enabled) are fetched before compiling
$(APP_OBJS): | $(BUILD_DIR) $(SDK_STAMP)
ifeq ($(BMP_ENABLE),1)
$(BMP_OBJS): | $(BUILD_DIR) $(SDK_STAMP) $(BMP_STAMP) $(BMP_VERSION_H)
endif

$(SDK_STAMP):
	$(ECHO) "SDK not found. Fetching MSP432E4 DriverLib..."
	$(Q)$(PYTHON) scripts/fetch_sdk.py

$(BMP_STAMP):
	$(ECHO) "Black Magic Debug sources not found. Fetching main branch..."
	$(Q)$(PYTHON) scripts/fetch_bmp.py

$(BMP_VERSION_H): $(BMP_STAMP) | $(BUILD_DIR)
	$(Q)mkdir -p $(dir $@)
	$(Q)VCS_TAG=$$(git -C third_party/blackmagic describe --always --dirty 2>/dev/null || echo "v2.0-openfw"); \
		printf '#define FIRMWARE_VERSION "v2.0-%s"\n' "$$VCS_TAG" > $@

$(DL_SRCS): | $(SDK_STAMP)
	@if [ ! -f $@ ]; then \
		echo "Missing SDK source: $@"; \
		exit 1; \
	fi

check-sdk: $(SDK_STAMP)

fetch-sdk sdk:
	$(Q)$(PYTHON) scripts/fetch_sdk.py

fetch-bmp bmp:
	$(Q)$(PYTHON) scripts/fetch_bmp.py

fetch-toolchain toolchain:
	$(Q)$(PYTHON) scripts/fetch_toolchain.py

$(BUILD_DIR):
	$(Q)mkdir -p $(BUILD_DIR)/src
	$(Q)mkdir -p $(BUILD_DIR)/sdk/ti/devices/msp432e4/driverlib

$(BUILD_DIR)/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "  CC    $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(BMP_ENABLE),1)
$(BMP_OBJS): $(BUILD_DIR)/%.o: %.c
	$(Q)mkdir -p $(dir $@)
	$(ECHO) "  BMP   $<"
	$(Q)$(CC) $(BMP_CFLAGS) -c $< -o $@
endif

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

SWD_TARGET_CFG ?= target/stm32g0x.cfg

test-hil:
	$(Q)$(PYTHON) tests/hil_stress_test.py

test-swd:
	$(Q)$(PYTHON) tests/swd_target_test.py --target-cfg $(SWD_TARGET_CFG)

clean:
	$(Q)rm -rf $(BUILD_DIR) compile_commands.json

-include $(DEPS)
