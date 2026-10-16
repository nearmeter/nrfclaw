#ifndef NRFCLAW_SHT_H
#define NRFCLAW_SHT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_SHT_FAMILY_NONE = 0,
    NRFCLAW_SHT_FAMILY_SHT2X = 1,
    NRFCLAW_SHT_FAMILY_SHT3X = 2,
    NRFCLAW_SHT_FAMILY_SHT4X = 3,
    NRFCLAW_SHT_FAMILY_SHTC3 = 4
} nrfclaw_sht_family_t;

typedef enum {
    NRFCLAW_SHT_OK = 0,
    NRFCLAW_SHT_BAD_ARG,
    NRFCLAW_SHT_BUSY,
    NRFCLAW_SHT_NOT_FOUND,
    NRFCLAW_SHT_IO_ERROR
} nrfclaw_sht_status_t;

typedef struct {
    nrfclaw_sht_family_t family;
    uint8_t address;
    uint8_t scl_pin;
    uint8_t sda_pin;
    uint32_t serial;
    bool serial_valid;
} nrfclaw_sht_info_t;

typedef struct {
    int32_t temperature_mC;
    uint32_t humidity_mpermille; /* 0..100000 = 0..100.000 %RH */
} nrfclaw_sht_sample_t;

nrfclaw_sht_status_t nrfclaw_sht_configure_default(void);
nrfclaw_sht_status_t nrfclaw_sht_configure(uint8_t scl_pin,uint8_t sda_pin);
nrfclaw_sht_status_t nrfclaw_sht_probe(void);
nrfclaw_sht_status_t nrfclaw_sht_read(nrfclaw_sht_sample_t *out);

void nrfclaw_sht_release(void);
bool nrfclaw_sht_active(void);
bool nrfclaw_sht_present(void);
bool nrfclaw_sht_owns_pin(uint8_t pin);
void nrfclaw_sht_info(nrfclaw_sht_info_t *out);

uint8_t nrfclaw_sht_crc8(uint8_t const *data,uint8_t len);

#endif
