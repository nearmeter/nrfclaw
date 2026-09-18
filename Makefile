# nRFClaw standalone build pack 02 (base: validated 01-r3)
.DEFAULT_GOAL := all

# Default board is the validated NINASENSE Rev 1.1.
#
# Persistent local configuration:
#   make configure BOARD=ninasense TOOLCHAIN_PATH=/path/to/bin NRF5SDK=vendor/nrf5sdk
# Afterwards:
#   make clean && make

LOCAL_CONFIG ?= .nrfclaw.mk
-include $(LOCAL_CONFIG)

BOARD ?= ninasense
BUILD_DIR ?= build/$(BOARD)
NRF5SDK ?= vendor/nrf5sdk

BOARD_MK := boards/$(BOARD)/board.mk
ifeq ($(wildcard $(BOARD_MK)),)
$(error Unknown BOARD='$(BOARD)'. Available: $(notdir $(wildcard boards/*)))
endif
include $(BOARD_MK)

# Toolchain: point TOOLCHAIN_PATH at the directory containing arm-none-eabi-gcc.
# GNU_INSTALL_ROOT is accepted as an alias for Nordic SDK users.
ifeq ($(strip $(TOOLCHAIN_PATH)),)
TOOLCHAIN_PATH := $(GNU_INSTALL_ROOT)
endif
ifneq ($(strip $(TOOLCHAIN_PATH)),)
TC := $(patsubst %/,%,$(TOOLCHAIN_PATH))/
endif
CC      := $(TC)arm-none-eabi-gcc
AS      := $(TC)arm-none-eabi-gcc
OBJCOPY := $(TC)arm-none-eabi-objcopy
SIZE    := $(TC)arm-none-eabi-size

TARGET := $(BUILD_DIR)/nrfclaw_$(BOARD)
ELF := $(TARGET).out
HEX := $(TARGET).hex
BIN := $(TARGET).bin
MAP := $(TARGET).map

CORE_SRC := \
  src/main.c \
  src/nrfclaw_battery.c \
  src/nrfclaw_ble.c \
  src/nrfclaw_event.c \
  src/nrfclaw_inputs.c \
  src/nrfclaw_lora.c \
  src/nrfclaw_llcc68_rl.c \
  src/nrfclaw_lora_profile.c \
  src/nrfclaw_lora_profile_store.c \
  src/nrfclaw_rtc.c \
  src/nrfclaw_vm.c \
  src/nrfclaw_auth.c \
  src/nrfclaw_flash.c \
  src/nrfclaw_schedule.c \
  src/nrfclaw_scheduler.c \
  src/nrfclaw_native.c \
  src/nrfclaw_capability.c \
  src/nrfclaw_ninalink.c \
  src/nrfclaw_ninalink_msg.c \
  src/nrfclaw_ninalink_lab.c \
  src/nrfclaw_ninalink_bridge.c \
  src/nrfclaw_system_power.c \
  src/nrfclaw_lis2dh12.c \
  src/nrfclaw_vib_health.c \
  src/nrfclaw_vib_auto.c \
  src/nrfclaw_vib_auto_store.c \
  src/nrfclaw_ble_app.c \
  src/nrfclaw_ble_boot.c \
  src/nrfclaw_state.c \
  src/nrfclaw_factory.c \
  src/nrfclaw_serial.c \
  src/nrfclaw_tracking.c \
  src/nrfclaw_sha256.c \
  src/nrfclaw_ndp.c \
  src/nrfclaw_ndp_access.c \
  src/nrfclaw_ndp_key_store.c \
  src/nrfclaw_ds18b20.c \
  src/nrfclaw_board_api.c

SDK_SRC := \
  $(NRF5SDK)/modules/nrfx/mdk/gcc_startup_nrf52.S \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_backend_rtt.c \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_backend_serial.c \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_backend_uart.c \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_default_backends.c \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_frontend.c \
  $(NRF5SDK)/components/libraries/log/src/nrf_log_str_formatter.c \
  $(NRF5SDK)/components/libraries/button/app_button.c \
  $(NRF5SDK)/components/libraries/util/app_error.c \
  $(NRF5SDK)/components/libraries/util/app_error_handler_gcc.c \
  $(NRF5SDK)/components/libraries/util/app_error_weak.c \
  $(NRF5SDK)/components/libraries/scheduler/app_scheduler.c \
  $(NRF5SDK)/components/libraries/timer/app_timer2.c \
  $(NRF5SDK)/components/libraries/fifo/app_fifo.c \
  $(NRF5SDK)/components/libraries/uart/app_uart_fifo.c \
  $(NRF5SDK)/components/libraries/util/app_util_platform.c \
  $(NRF5SDK)/components/libraries/crc16/crc16.c \
  $(NRF5SDK)/components/libraries/timer/drv_rtc.c \
  $(NRF5SDK)/components/libraries/fds/fds.c \
  $(NRF5SDK)/components/libraries/hardfault/hardfault_implementation.c \
  $(NRF5SDK)/components/libraries/util/nrf_assert.c \
  $(NRF5SDK)/components/libraries/atomic_fifo/nrf_atfifo.c \
  $(NRF5SDK)/components/libraries/atomic_flags/nrf_atflags.c \
  $(NRF5SDK)/components/libraries/atomic/nrf_atomic.c \
  $(NRF5SDK)/components/libraries/balloc/nrf_balloc.c \
  $(NRF5SDK)/components/libraries/queue/nrf_queue.c \
  $(NRF5SDK)/external/fprintf/nrf_fprintf.c \
  $(NRF5SDK)/external/fprintf/nrf_fprintf_format.c \
  $(NRF5SDK)/components/libraries/fstorage/nrf_fstorage.c \
  $(NRF5SDK)/components/libraries/fstorage/nrf_fstorage_sd.c \
  $(NRF5SDK)/components/libraries/memobj/nrf_memobj.c \
  $(NRF5SDK)/components/libraries/pwr_mgmt/nrf_pwr_mgmt.c \
  $(NRF5SDK)/components/libraries/ringbuf/nrf_ringbuf.c \
  $(NRF5SDK)/components/libraries/experimental_section_vars/nrf_section_iter.c \
  $(NRF5SDK)/components/libraries/sortlist/nrf_sortlist.c \
  $(NRF5SDK)/components/libraries/strerror/nrf_strerror.c \
  $(NRF5SDK)/integration/nrfx/legacy/nrf_drv_spi.c \
  $(NRF5SDK)/components/libraries/sensorsim/sensorsim.c \
  $(NRF5SDK)/modules/nrfx/mdk/system_nrf52.c \
  $(NRF5SDK)/components/boards/boards.c \
  $(NRF5SDK)/integration/nrfx/legacy/nrf_drv_clock.c \
  $(NRF5SDK)/integration/nrfx/legacy/nrf_drv_uart.c \
  $(NRF5SDK)/integration/nrfx/legacy/nrf_drv_rng.c \
  $(NRF5SDK)/modules/nrfx/soc/nrfx_atomic.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_clock.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_gpiote.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_saadc.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/prs/nrfx_prs.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_spi.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_spim.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_uart.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_uarte.c \
  $(NRF5SDK)/modules/nrfx/drivers/src/nrfx_rng.c \
  $(NRF5SDK)/components/libraries/bsp/bsp.c \
  $(NRF5SDK)/components/libraries/bsp/bsp_btn_ble.c \
  $(NRF5SDK)/external/micro-ecc/micro-ecc/uECC.c \
  $(NRF5SDK)/external/segger_rtt/SEGGER_RTT.c \
  $(NRF5SDK)/external/segger_rtt/SEGGER_RTT_Syscalls_GCC.c \
  $(NRF5SDK)/external/segger_rtt/SEGGER_RTT_printf.c \
  $(NRF5SDK)/components/ble/peer_manager/auth_status_tracker.c \
  $(NRF5SDK)/components/ble/common/ble_advdata.c \
  $(NRF5SDK)/components/ble/common/ble_conn_params.c \
  $(NRF5SDK)/components/ble/common/ble_conn_state.c \
  $(NRF5SDK)/components/ble/common/ble_srv_common.c \
  $(NRF5SDK)/components/ble/ble_services/ble_nus/ble_nus.c \
  $(NRF5SDK)/components/ble/peer_manager/gatt_cache_manager.c \
  $(NRF5SDK)/components/ble/peer_manager/gatts_cache_manager.c \
  $(NRF5SDK)/components/ble/peer_manager/id_manager.c \
  $(NRF5SDK)/components/ble/nrf_ble_gatt/nrf_ble_gatt.c \
  $(NRF5SDK)/components/ble/nrf_ble_qwr/nrf_ble_qwr.c \
  $(NRF5SDK)/components/ble/ble_link_ctx_manager/ble_link_ctx_manager.c \
  $(NRF5SDK)/components/ble/peer_manager/peer_data_storage.c \
  $(NRF5SDK)/components/ble/peer_manager/peer_database.c \
  $(NRF5SDK)/components/ble/peer_manager/peer_id.c \
  $(NRF5SDK)/components/ble/peer_manager/peer_manager.c \
  $(NRF5SDK)/components/ble/peer_manager/peer_manager_handler.c \
  $(NRF5SDK)/components/ble/peer_manager/pm_buffer.c \
  $(NRF5SDK)/components/ble/peer_manager/security_dispatcher.c \
  $(NRF5SDK)/components/ble/peer_manager/security_manager.c \
  $(NRF5SDK)/external/utf_converter/utf.c \
  $(NRF5SDK)/components/softdevice/common/nrf_sdh.c \
  $(NRF5SDK)/components/softdevice/common/nrf_sdh_ble.c \
  $(NRF5SDK)/components/softdevice/common/nrf_sdh_soc.c

SRC := $(CORE_SRC) $(SDK_SRC)
ASM_SRC := $(filter %.S,$(SRC))
C_SRC := $(filter %.c,$(SRC))
OBJ := $(addprefix $(BUILD_DIR)/obj/,$(C_SRC:.c=.o)) $(addprefix $(BUILD_DIR)/obj/,$(ASM_SRC:.S=.o))

INC := \
  include \
  config \
  boards/$(BOARD) \
  $(NRF5SDK)/external/micro-ecc/micro-ecc \
  $(NRF5SDK)/components/nfc/ndef/generic/message \
  $(NRF5SDK)/components/nfc/t2t_lib \
  $(NRF5SDK)/components/nfc/t4t_parser/hl_detection_procedure \
  $(NRF5SDK)/components/ble/ble_services/ble_ancs_c \
  $(NRF5SDK)/components/ble/ble_services/ble_ias_c \
  $(NRF5SDK)/components/libraries/pwm \
  $(NRF5SDK)/components/softdevice/s132/headers/nrf52 \
  $(NRF5SDK)/components/libraries/usbd/class/cdc/acm \
  $(NRF5SDK)/components/libraries/usbd/class/hid/generic \
  $(NRF5SDK)/components/libraries/usbd/class/msc \
  $(NRF5SDK)/components/libraries/usbd/class/hid \
  $(NRF5SDK)/modules/nrfx/hal \
  $(NRF5SDK)/components/nfc/ndef/conn_hand_parser/le_oob_rec_parser \
  $(NRF5SDK)/components/libraries/log \
  $(NRF5SDK)/components/ble/ble_services/ble_gls \
  $(NRF5SDK)/components/libraries/fstorage \
  $(NRF5SDK)/components/nfc/ndef/text \
  $(NRF5SDK)/components/libraries/mutex \
  $(NRF5SDK)/components/libraries/gpiote \
  $(NRF5SDK)/components/libraries/fifo \
  $(NRF5SDK)/components/libraries/bootloader/ble_dfu \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/common \
  $(NRF5SDK)/components/boards \
  $(NRF5SDK)/components/nfc/ndef/generic/record \
  $(NRF5SDK)/components/nfc/t4t_parser/cc_file \
  $(NRF5SDK)/components/ble/ble_advertising \
  $(NRF5SDK)/components/ble/ble_link_ctx_manager \
  $(NRF5SDK)/external/utf_converter \
  $(NRF5SDK)/components/ble/ble_services/ble_bas_c \
  $(NRF5SDK)/modules/nrfx/drivers/include \
  $(NRF5SDK)/components/libraries/experimental_task_manager \
  $(NRF5SDK)/components/ble/ble_services/ble_hrs_c \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/le_oob_rec \
  $(NRF5SDK)/components/libraries/queue \
  $(NRF5SDK)/components/libraries/pwr_mgmt \
  $(NRF5SDK)/components/ble/ble_dtm \
  $(NRF5SDK)/components/toolchain/cmsis/include \
  $(NRF5SDK)/components/ble/ble_services/ble_rscs_c \
  $(NRF5SDK)/components/ble/common \
  $(NRF5SDK)/components/ble/ble_services/ble_lls \
  $(NRF5SDK)/components/nfc/platform \
  $(NRF5SDK)/components/libraries/bsp \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/ac_rec \
  $(NRF5SDK)/components/ble/ble_services/ble_bas \
  $(NRF5SDK)/components/libraries/mpu \
  $(NRF5SDK)/components/libraries/experimental_section_vars \
  $(NRF5SDK)/components/softdevice/s132/headers \
  $(NRF5SDK)/components/ble/ble_services/ble_ans_c \
  $(NRF5SDK)/components/libraries/slip \
  $(NRF5SDK)/components/libraries/delay \
  $(NRF5SDK)/components/libraries/csense_drv \
  $(NRF5SDK)/components/libraries/memobj \
  $(NRF5SDK)/components/ble/ble_services/ble_nus_c \
  $(NRF5SDK)/components/softdevice/common \
  $(NRF5SDK)/components/ble/ble_services/ble_ias \
  $(NRF5SDK)/components/libraries/usbd/class/hid/mouse \
  $(NRF5SDK)/components/libraries/low_power_pwm \
  $(NRF5SDK)/components/nfc/ndef/conn_hand_parser/ble_oob_advdata_parser \
  $(NRF5SDK)/components/ble/ble_services/ble_dfu \
  $(NRF5SDK)/external/fprintf \
  $(NRF5SDK)/components/libraries/svc \
  $(NRF5SDK)/components/libraries/atomic \
  $(NRF5SDK)/components \
  $(NRF5SDK)/components/libraries/scheduler \
  $(NRF5SDK)/components/libraries/cli \
  $(NRF5SDK)/components/ble/ble_services/ble_lbs \
  $(NRF5SDK)/components/ble/ble_services/ble_hts \
  $(NRF5SDK)/components/libraries/crc16 \
  $(NRF5SDK)/components/nfc/t4t_parser/apdu \
  $(NRF5SDK)/components/libraries/util \
  $(NRF5SDK)/components/libraries/usbd/class/cdc \
  $(NRF5SDK)/components/libraries/csense \
  $(NRF5SDK)/components/libraries/balloc \
  $(NRF5SDK)/components/libraries/ecc \
  $(NRF5SDK)/components/libraries/hardfault \
  $(NRF5SDK)/components/ble/ble_services/ble_cscs \
  $(NRF5SDK)/components/libraries/uart \
  $(NRF5SDK)/components/libraries/hci \
  $(NRF5SDK)/components/libraries/timer \
  $(NRF5SDK)/integration/nrfx \
  $(NRF5SDK)/components/nfc/t4t_parser/tlv \
  $(NRF5SDK)/components/libraries/sortlist \
  $(NRF5SDK)/components/libraries/spi_mngr \
  $(NRF5SDK)/components/libraries/led_softblink \
  $(NRF5SDK)/components/nfc/ndef/conn_hand_parser \
  $(NRF5SDK)/components/libraries/sdcard \
  $(NRF5SDK)/components/nfc/ndef/parser/record \
  $(NRF5SDK)/modules/nrfx/mdk \
  $(NRF5SDK)/components/ble/ble_services/ble_cts_c \
  $(NRF5SDK)/components/ble/ble_services/ble_nus \
  $(NRF5SDK)/components/libraries/twi_mngr \
  $(NRF5SDK)/components/ble/ble_services/ble_hids \
  $(NRF5SDK)/components/libraries/strerror \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/ble_oob_advdata \
  $(NRF5SDK)/components/nfc/t2t_parser \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/ble_pair_msg \
  $(NRF5SDK)/components/libraries/usbd/class/audio \
  $(NRF5SDK)/components/libraries/sensorsim \
  $(NRF5SDK)/components/nfc/t4t_lib \
  $(NRF5SDK)/components/ble/peer_manager \
  $(NRF5SDK)/components/libraries/mem_manager \
  $(NRF5SDK)/components/libraries/ringbuf \
  $(NRF5SDK)/components/ble/ble_services/ble_tps \
  $(NRF5SDK)/components/nfc/ndef/parser/message \
  $(NRF5SDK)/components/ble/ble_services/ble_dis \
  $(NRF5SDK)/components/nfc/ndef/uri \
  $(NRF5SDK)/components/ble/nrf_ble_gatt \
  $(NRF5SDK)/components/ble/nrf_ble_qwr \
  $(NRF5SDK)/components/libraries/gfx \
  $(NRF5SDK)/components/libraries/button \
  $(NRF5SDK)/modules/nrfx \
  $(NRF5SDK)/components/libraries/twi_sensor \
  $(NRF5SDK)/integration/nrfx/legacy \
  $(NRF5SDK)/components/libraries/usbd/class/hid/kbd \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/ep_oob_rec \
  $(NRF5SDK)/external/segger_rtt \
  $(NRF5SDK)/components/libraries/atomic_fifo \
  $(NRF5SDK)/components/ble/ble_services/ble_lbs_c \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/ble_pair_lib \
  $(NRF5SDK)/components/libraries/crypto \
  $(NRF5SDK)/components/ble/ble_racp \
  $(NRF5SDK)/components/libraries/fds \
  $(NRF5SDK)/components/nfc/ndef/launchapp \
  $(NRF5SDK)/components/libraries/atomic_flags \
  $(NRF5SDK)/components/ble/ble_services/ble_hrs \
  $(NRF5SDK)/components/ble/ble_services/ble_rscs \
  $(NRF5SDK)/components/nfc/ndef/connection_handover/hs_rec \
  $(NRF5SDK)/components/libraries/usbd \
  $(NRF5SDK)/components/nfc/ndef/conn_hand_parser/ac_rec_parser \
  $(NRF5SDK)/components/libraries/stack_guard \
  $(NRF5SDK)/components/libraries/log/src
CPPFLAGS := $(addprefix -I,$(INC)) $(BOARD_CPPFLAGS)
CPPFLAGS += -DAPP_TIMER_V2 -DAPP_TIMER_V2_RTC1_ENABLED
CPPFLAGS += -DFLOAT_ABI_HARD -DNRF52 -D$(SOC_DEFINE) -DNRF52_PAN_74
CPPFLAGS += -DNRF_SD_BLE_API_VERSION=7 -DS132 -DCONFIG_NFCT_PINS_AS_GPIOS -DSOFTDEVICE_PRESENT
CPPFLAGS += -DuECC_ENABLE_VLI_API=0 -DuECC_OPTIMIZATION_LEVEL=3 -DuECC_SQUARE_FUNC=0
CPPFLAGS += -DuECC_SUPPORT_COMPRESSED_POINT=0 -DuECC_VLI_NATIVE_LITTLE_ENDIAN=1
CPPFLAGS += -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 -DuECC_SUPPORTS_secp224r1=1
CPPFLAGS += -DuECC_SUPPORTS_secp256r1=0 -DuECC_SUPPORTS_secp256k1=0
CPPFLAGS += -D__HEAP_SIZE=8192 -D__STACK_SIZE=8192

ARCH := -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
OPT ?= -O3 -g3
CFLAGS := $(ARCH) $(OPT) -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-builtin -fshort-enums
ASFLAGS := $(ARCH) -g3 -x assembler-with-cpp
LDFLAGS := $(ARCH) $(OPT) -T$(LINKER_SCRIPT) -L$(NRF5SDK)/modules/nrfx/mdk -Wl,-Map=$(MAP) -Wl,--gc-sections --specs=nano.specs
LDLIBS := -lc -lnosys -lm -lgcc

# Save machine-local build settings. This file should not be committed.
configure:
	@printf '%s\n' \
	  '# Local nRFClaw build configuration - generated by make configure' \
	  'BOARD ?= $(BOARD)' \
	  'TOOLCHAIN_PATH ?= $(TOOLCHAIN_PATH)' \
	  'NRF5SDK ?= $(NRF5SDK)' \
	  > $(LOCAL_CONFIG)
	@echo "Saved $(LOCAL_CONFIG):"
	@cat $(LOCAL_CONFIG)

show-config:
	@echo "BOARD=$(BOARD)"
	@echo "LFCLK=$(NRFCLAW_LFCLK)"
	@echo "DCDC=$(NRFCLAW_DCDC)"
	@echo "TOOLCHAIN_PATH=$(TOOLCHAIN_PATH)"
	@echo "NRF5SDK=$(NRF5SDK)"
	@echo "LOCAL_CONFIG=$(LOCAL_CONFIG)"

.PHONY: configure show-config all default clean help boards check-toolchain check-vendor flash flash_softdevice erase reset recover size bootloader install_bootloader
default: all
all: check-toolchain check-vendor $(HEX) $(BIN)

help:
	@echo 'nRFClaw public firmware build'
	@echo '  make BOARD=ninasense TOOLCHAIN_PATH=/path/to/bin'
	@echo '  make BOARD=pca10040 TOOLCHAIN_PATH=/path/to/bin'
	@echo '  make BOARD=halfmoon TOOLCHAIN_PATH=/path/to/bin'
	@echo '  make flash BOARD=ninasense'
	@echo '  make flash_softdevice BOARD=ninasense'
	@echo '  make boards'
	@echo '  make bootloader BOARD=ninasense'
	@echo '  make install_bootloader BOARD=ninasense  # ONE-TIME J-Link factory install'

boards:
	@for d in boards/*; do [ -f "$$d/board.mk" ] && echo $${d#boards/}; done

check-toolchain:
	@command -v $(CC) >/dev/null 2>&1 || { echo 'ERROR: arm-none-eabi-gcc not found. Set TOOLCHAIN_PATH=/path/to/toolchain/bin'; exit 2; }

check-vendor:
	@test -f $(NRF5SDK)/.imported || { echo 'ERROR: vendor/nrf5sdk is not initialized.'; echo 'Populate the curated vendor/nrf5sdk tree and create vendor/nrf5sdk/.imported.'; exit 2; }


$(BUILD_DIR)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(CPPFLAGS) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJ) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@
	$(SIZE) $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

size: $(ELF)
	$(SIZE) $(ELF)

clean:
	rm -rf build

NRFJPROG ?= nrfjprog
NRF_FAMILY ?= nrf52
NRF_SPEED ?= 50000
NRF_RETRIES ?= 10
define NRFJPROG_RETRY
	@attempt=1; while [ $$attempt -le $(NRF_RETRIES) ]; do \
	 echo "[J-Link] attempt $$attempt/$(NRF_RETRIES): $(NRFJPROG) -f $(NRF_FAMILY) --clockspeed $(NRF_SPEED) $(1)"; \
	 $(NRFJPROG) -f $(NRF_FAMILY) --clockspeed $(NRF_SPEED) $(1) && exit 0; \
	 [ $$attempt -ge $(NRF_RETRIES) ] && exit 1; attempt=$$((attempt+1)); done
endef

flash: all
	$(call NRFJPROG_RETRY,--program $(HEX) --sectorerase)
	$(call NRFJPROG_RETRY,--reset)

flash_softdevice: check-vendor
	@test -f $(SOFTDEVICE_HEX) || { echo 'ERROR: missing $(SOFTDEVICE_HEX)'; exit 2; }
	$(call NRFJPROG_RETRY,--program $(SOFTDEVICE_HEX) --sectorerase)
	$(call NRFJPROG_RETRY,--reset)

erase:
	$(call NRFJPROG_RETRY,--eraseall)
reset:
	$(call NRFJPROG_RETRY,--reset)
recover:
	$(call NRFJPROG_RETRY,--recover)

bootloader: check-toolchain check-vendor
	$(MAKE) -C bootloader NRF5SDK=../$(NRF5SDK) TOOLCHAIN_PATH=$(TOOLCHAIN_PATH)

install_bootloader: bootloader
	@echo 'ONE-TIME factory operation: program bootloader and UICR BOOTLOADERADDR.'
	$(call NRFJPROG_RETRY,--program bootloader/build/nrfclaw_bootloader.hex --sectorerase)
	$(call NRFJPROG_RETRY,--memwr 0x10001014 --val 0x0007A000)
	$(call NRFJPROG_RETRY,--reset)

-include $(OBJ:.o=.d)
