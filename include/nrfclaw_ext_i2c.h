#ifndef NRFCLAW_EXT_I2C_H
#define NRFCLAW_EXT_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrfclaw_board.h"

/*
 * B8.1k8b1 — external low-power software I2C using internal GPIO pull-ups.
 *
 * NINASENSE default:
 *   SCL = D11 / P0.11
 *   SDA = D14 / P0.14
 *
 * The caller may explicitly configure another valid GPIO pair.
 *
 * This layer does NOT use TWI0/TWI1, so the onboard LIS2DH12 keeps its
 * independent hardware-TWI path.
 *
 * Electrical model:
 *   logic LOW  -> GPIO output-low
 *   logic HIGH -> GPIO input + internal PULLUP
 *
 * No external I2C pull-up resistors are required for the intended short-wire
 * SHT connection.
 */

#ifndef P_SHT_SCL_DEFAULT
#define P_SHT_SCL_DEFAULT P_HALL1
#endif

#ifndef P_SHT_SDA_DEFAULT
#define P_SHT_SDA_DEFAULT P_HALL2
#endif

/* Conservative software-I2C rate for weak internal pull-ups and short wires. */
#define NRFCLAW_EXT_I2C_HALF_PERIOD_US 10U
#define NRFCLAW_EXT_I2C_STRETCH_TIMEOUT_US 1000U

typedef struct {
    uint8_t scl_pin;
    uint8_t sda_pin;
    bool configured;
    bool busy;
} nrfclaw_ext_i2c_t;

bool nrfclaw_ext_i2c_configure(nrfclaw_ext_i2c_t *bus,
                               uint8_t scl_pin,
                               uint8_t sda_pin);

bool nrfclaw_ext_i2c_configure_sht_default(nrfclaw_ext_i2c_t *bus);

/* Keep configured pins released HIGH through internal pull-ups. */
void nrfclaw_ext_i2c_idle(nrfclaw_ext_i2c_t *bus);

/* Fully release ownership: true high-Z, no pull. */
void nrfclaw_ext_i2c_deinit(nrfclaw_ext_i2c_t *bus);

bool nrfclaw_ext_i2c_recover(nrfclaw_ext_i2c_t *bus);

bool nrfclaw_ext_i2c_write(nrfclaw_ext_i2c_t *bus,
                           uint8_t address7,
                           uint8_t const *data,
                           size_t len);

bool nrfclaw_ext_i2c_read(nrfclaw_ext_i2c_t *bus,
                          uint8_t address7,
                          uint8_t *data,
                          size_t len);

bool nrfclaw_ext_i2c_write_read(nrfclaw_ext_i2c_t *bus,
                                uint8_t address7,
                                uint8_t const *write_data,
                                size_t write_len,
                                uint8_t *read_data,
                                size_t read_len);

#endif
