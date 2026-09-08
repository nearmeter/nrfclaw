#ifndef NRFCLAW_NDP_KEY_STORE_H
#define NRFCLAW_NDP_KEY_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_NDP_KEY_SIZE              32U
#define NRFCLAW_NDP_KEY_SLOT_A_ADDR       0x0006C000UL
#define NRFCLAW_NDP_KEY_SLOT_B_ADDR       0x0006D000UL

typedef enum {
    NRFCLAW_NDP_KEY_UNCONFIGURED = 0,
    NRFCLAW_NDP_KEY_READY        = 1,
    NRFCLAW_NDP_KEY_SAVING       = 2,
    NRFCLAW_NDP_KEY_ERROR        = 3
} nrfclaw_ndp_key_status_t;

void nrfclaw_ndp_key_store_init(void);
void nrfclaw_ndp_key_store_process(void);

bool nrfclaw_ndp_key_configured(void);
nrfclaw_ndp_key_status_t nrfclaw_ndp_key_status(void);
const uint8_t *nrfclaw_ndp_key(void);

/* Generates a new 256-bit key inside the device and starts an atomic A/B save. */
bool nrfclaw_ndp_key_generate(void);

#endif
