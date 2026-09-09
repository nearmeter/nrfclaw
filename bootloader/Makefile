NRF5SDK ?= ../vendor/nrf5sdk
TOOLCHAIN_PATH ?=
TC := $(patsubst %/,%,$(TOOLCHAIN_PATH))/
ifeq ($(strip $(TOOLCHAIN_PATH)),)
TC :=
endif
CC=$(TC)arm-none-eabi-gcc
OBJCOPY=$(TC)arm-none-eabi-objcopy
SIZE=$(TC)arm-none-eabi-size
BUILD=build
TARGET=$(BUILD)/nrfclaw_bootloader
LINKER=nrf52832_s132_bootloader.ld

# Board profile used by the bootloader for hardware-owned recovery inputs.
# The selected boards/<board>/board_config.h supplies P_BUTTON; the bootloader
# must never hard-code the NINASENSE P0.21 mapping.
BOARD ?= ninasense
BOARD_DIR ?= ../boards/$(BOARD)
BOARD_CONFIG := $(BOARD_DIR)/board_config.h

ifeq ($(wildcard $(BOARD_CONFIG)),)
$(error Missing board profile: $(BOARD_CONFIG))
endif
SRC=main.c \
 $(NRF5SDK)/modules/nrfx/mdk/gcc_startup_nrf52.S \
 $(NRF5SDK)/modules/nrfx/mdk/system_nrf52.c \
 $(NRF5SDK)/components/softdevice/common/nrf_sdh.c \
 $(NRF5SDK)/components/softdevice/common/nrf_sdh_ble.c \
 $(NRF5SDK)/components/softdevice/common/nrf_sdh_soc.c \
 $(NRF5SDK)/components/ble/common/ble_advdata.c \
 $(NRF5SDK)/components/ble/common/ble_srv_common.c \
 $(NRF5SDK)/components/ble/common/ble_conn_state.c \
 $(NRF5SDK)/components/ble/ble_link_ctx_manager/ble_link_ctx_manager.c \
 $(NRF5SDK)/components/libraries/atomic/nrf_atomic.c \
 $(NRF5SDK)/components/libraries/atomic_flags/nrf_atflags.c \
 $(NRF5SDK)/components/libraries/strerror/nrf_strerror.c \
 $(NRF5SDK)/components/libraries/util/app_error.c \
 $(NRF5SDK)/components/libraries/util/app_error_weak.c \
 $(NRF5SDK)/components/libraries/util/app_util_platform.c \
 $(NRF5SDK)/components/libraries/util/nrf_assert.c \
 $(NRF5SDK)/components/libraries/experimental_section_vars/nrf_section_iter.c
INC=../include ../config $(BOARD_DIR) \
 $(NRF5SDK)/modules/nrfx/mdk $(NRF5SDK)/modules/nrfx/hal $(NRF5SDK)/modules/nrfx \
 $(NRF5SDK)/modules/nrfx/templates/nRF52832 \
 $(NRF5SDK)/integration/nrfx \
 $(NRF5SDK)/components $(NRF5SDK)/components/toolchain/cmsis/include \
 $(NRF5SDK)/components/softdevice/s132/headers $(NRF5SDK)/components/softdevice/s132/headers/nrf52 \
 $(NRF5SDK)/components/softdevice/common $(NRF5SDK)/components/ble/common \
 $(NRF5SDK)/components/ble/ble_link_ctx_manager \
 $(NRF5SDK)/components/ble/ble_services/ble_nus $(NRF5SDK)/components/libraries/atomic \
 $(NRF5SDK)/components/libraries/atomic_flags \
 $(NRF5SDK)/components/libraries/strerror \
 $(NRF5SDK)/components/libraries/util \
 $(NRF5SDK)/components/libraries/delay \
 $(NRF5SDK)/components/libraries/log \
 $(NRF5SDK)/components/libraries/log/src \
 $(NRF5SDK)/external/segger_rtt \
 $(NRF5SDK)/components/libraries/experimental_section_vars
CPPFLAGS=$(addprefix -I,$(INC)) -DNRF52832_XXAA -DNRF52 -DS132 -DSOFTDEVICE_PRESENT -DNRF_SD_BLE_API_VERSION=7 \
 -DNRF_SDH_BLE_ENABLED=1 -DNRF_SDH_ENABLED=1 -DNRF_SDH_SOC_ENABLED=1 \
 -DNRF_SDH_BLE_PERIPHERAL_LINK_COUNT=1 -DNRF_SDH_BLE_CENTRAL_LINK_COUNT=0 -DNRF_SDH_BLE_TOTAL_LINK_COUNT=1 \
 -DNRF_SDH_BLE_GATT_MAX_MTU_SIZE=247 -DNRF_SDH_BLE_GAP_DATA_LENGTH=251 -DNRF_LOG_ENABLED=0 \
 -DNRF_SDH_BLE_OBSERVER_PRIO_LEVELS=4 -DNRF_SDH_SOC_OBSERVER_PRIO_LEVELS=2 \
 -DNRF_SDH_DISPATCH_MODEL=0 -DNRF_SDH_CLOCK_LF_SRC=1 -DNRF_SDH_CLOCK_LF_RC_CTIV=0 -DNRF_SDH_CLOCK_LF_RC_TEMP_CTIV=0 -DNRF_SDH_CLOCK_LF_ACCURACY=7 \
 -D__HEAP_SIZE=0 -D__STACK_SIZE=4096
ARCH=-mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS=$(ARCH) -Os -g2 -ffunction-sections -fdata-sections -fno-strict-aliasing -fshort-enums
LDFLAGS=$(ARCH) -Os -T$(LINKER) -L$(NRF5SDK)/modules/nrfx/mdk -Wl,-Map=$(TARGET).map -Wl,--gc-sections --specs=nano.specs
C_SRC=$(filter %.c,$(SRC))
S_SRC=$(filter %.S,$(SRC))
OBJ=$(addprefix $(BUILD)/,$(notdir $(C_SRC:.c=.o))) $(addprefix $(BUILD)/,$(notdir $(S_SRC:.S=.o)))
vpath %.c $(sort $(dir $(C_SRC)))
vpath %.S $(sort $(dir $(S_SRC)))
all: $(TARGET).hex $(TARGET).bin

show-config:
	@echo "BOARD=$(BOARD)"
	@echo "BOARD_CONFIG=$(BOARD_CONFIG)"
	@echo "NRF5SDK=$(NRF5SDK)"

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD); $(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(BUILD); $(CC) $(CPPFLAGS) $(ARCH) -x assembler-with-cpp -c $< -o $@
$(TARGET).out: $(OBJ) $(LINKER)
	@mkdir -p $(BUILD); $(CC) $(LDFLAGS) $(OBJ) -lc -lnosys -lgcc -o $@; $(SIZE) $@
	@python3 -c 'import os,sys; p="$(TARGET).bin"; sys.exit(0)' || true
$(TARGET).hex: $(TARGET).out
	$(OBJCOPY) -O ihex $< $@
$(TARGET).bin: $(TARGET).out
	$(OBJCOPY) -O binary $< $@
	@python3 -c 'import os,sys; n=os.path.getsize("$@"); print("bootloader bytes:",n); sys.exit(0 if n<=0x5000 else 2)'
clean:
	rm -rf $(BUILD)
.PHONY: all clean show-config
