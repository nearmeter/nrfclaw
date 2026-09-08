MCU := nrf52832
SOC_DEFINE := NRF52832_XXAA
SOFTDEVICE := s132
SOFTDEVICE_HEX := vendor/nrf5sdk/components/softdevice/s132/hex/s132_nrf52_7.2.0_softdevice.hex
LINKER_SCRIPT := boards/ninasense/nrf52832_s132.ld
# Compatibility define used only by Nordic SDK17 boards.c/BSP sources.
# nRFClaw pin mapping and board identity still come from
# boards/ninasense/board_config.h.
SDK_BOARD_COMPAT_DEFINE := BOARD_PCA10040
BOARD_CPPFLAGS := -D$(SDK_BOARD_COMPAT_DEFINE) -DNRFCLAW_BOARD_NINASENSE=1 \
                  -DNRFCLAW_EXPERIMENTAL_VIB_HEALTH=1 -DNRFCLAW_EXPERIMENTAL_VIB_AUTO=1

# Board power/clock profile. NINASENSE has a 32.768 kHz LF crystal and DCDC hardware.
NRFCLAW_LFCLK := XTAL
NRFCLAW_DCDC := 1
BOARD_CPPFLAGS += -DNRF_SDH_CLOCK_LF_SRC=1 -DNRF_SDH_CLOCK_LF_RC_CTIV=0 \
                  -DNRF_SDH_CLOCK_LF_RC_TEMP_CTIV=0 -DNRF_SDH_CLOCK_LF_ACCURACY=7 \
                  -DNRFCLAW_DCDC_ENABLE=1
