#ifndef NRFCLAW_NATIVE_H
#define NRFCLAW_NATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_inputs.h"
#include "nrfclaw_lis2dh12.h"

typedef enum {
    NRFCLAW_CAP_GPIO = 1,
    NRFCLAW_CAP_BATTERY = 2,
    NRFCLAW_CAP_DS18B20 = 3,
    NRFCLAW_CAP_ACCEL = 4,
    NRFCLAW_CAP_LORA = 5,
    NRFCLAW_CAP_BLE_APP = 6,
    NRFCLAW_CAP_RTC = 7,
    NRFCLAW_CAP_HALL = 8,
    NRFCLAW_CAP_STATE = 9,
    NRFCLAW_CAP_SERIAL = 10,
    NRFCLAW_CAP_TRACKING = 11,
    NRFCLAW_CAP_VIB_HEALTH = 12, /* EXPERIMENTAL */
    NRFCLAW_CAP_VIB_AUTO = 13,   /* EXPERIMENTAL */
    NRFCLAW_CAP_TEMPERATURE = 14 /* logical temperature provider */
} nrfclaw_capability_id_t;

typedef enum {
    NRFCLAW_NATIVE_OK = 0,
    NRFCLAW_NATIVE_BUSY,
    NRFCLAW_NATIVE_UNAVAILABLE,
    NRFCLAW_NATIVE_BAD_ARG,
    NRFCLAW_NATIVE_ERROR
} nrfclaw_native_status_t;

void nrfclaw_native_init(void);

/* Board/firmware discovery capability for NDP CAPS. Runtime enable state does not affect it. */
bool nrfclaw_native_capability_supported(uint8_t capability);

/* Current runtime state (for Hall/Serial/Tracking/Vib Auto and legacy CAP_AVAILABLE). */
bool nrfclaw_native_capability_active(uint8_t capability);

/* Backward-compatible alias for the current runtime state. */
bool nrfclaw_native_capability_available(uint8_t capability);

/* Explicit claims for shared connector resources. */
nrfclaw_native_status_t nrfclaw_native_hall_enable(bool pullup);
nrfclaw_native_status_t nrfclaw_native_hall_configure(nrfclaw_hall_config_t const *cfg);
void nrfclaw_native_hall_disable(void);

/* DS18B20 is auto-probed once at boot using 1-Wire presence detect on D20. */
bool nrfclaw_native_ds18_active(void);

nrfclaw_native_status_t nrfclaw_native_gpio_write(uint8_t pin, bool high);
nrfclaw_native_status_t nrfclaw_native_gpio_read(uint8_t pin, uint32_t *value);
nrfclaw_native_status_t nrfclaw_native_accel_xyz(int16_t *x, int16_t *y, int16_t *z);
nrfclaw_native_status_t nrfclaw_native_accel_motion(uint16_t threshold_mg, uint8_t duration_ticks);
nrfclaw_native_status_t nrfclaw_native_accel_configure(const nrfclaw_accel_config_t *cfg);

nrfclaw_native_status_t nrfclaw_native_serial_enable(uint8_t tx_pin,uint8_t rx_pin,uint32_t baud);
nrfclaw_native_status_t nrfclaw_native_serial_enable_economy(uint8_t tx_pin,uint8_t rx_pin,uint32_t baud);
void nrfclaw_native_serial_disable(void);
nrfclaw_native_status_t nrfclaw_native_serial_write(uint8_t const *data,uint16_t len);
nrfclaw_native_status_t nrfclaw_native_serial_read(uint32_t *value);

#endif
