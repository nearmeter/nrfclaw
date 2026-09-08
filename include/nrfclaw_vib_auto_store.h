#ifndef NRFCLAW_VIB_AUTO_STORE_H
#define NRFCLAW_VIB_AUTO_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_vib_auto.h"

#define NRFCLAW_VIB_AUTO_STORE_SLOT_A_ADDR 0x00078000UL
#define NRFCLAW_VIB_AUTO_STORE_SLOT_B_ADDR 0x00079000UL

typedef struct {
    uint8_t enabled;
    uint8_t discovery_complete;
    uint8_t profile_count;
    uint8_t reserved0;
    uint16_t quiet_rms_mg;
    uint16_t quiet_peak_mg;
    nrfclaw_vib_auto_config_t config;
    nrfclaw_vib_auto_profile_t profiles[NRFCLAW_VIB_AUTO_MAX_PROFILES];
} nrfclaw_vib_auto_persist_t;

void nrfclaw_vib_auto_store_init(void);
void nrfclaw_vib_auto_store_process(void);
bool nrfclaw_vib_auto_store_load(nrfclaw_vib_auto_persist_t *out);
bool nrfclaw_vib_auto_store_request_save(nrfclaw_vib_auto_persist_t const *in);
bool nrfclaw_vib_auto_store_busy(void);
uint32_t nrfclaw_vib_auto_store_generation(void);

#endif
