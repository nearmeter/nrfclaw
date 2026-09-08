#ifndef NRFCLAW_LORA_PROFILE_STORE_H
#define NRFCLAW_LORA_PROFILE_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_lora_profile.h"

/* R3.8.19a: relocated below application data area to reserve 0x7A000.. for bootloader. */
#define NRFCLAW_LORA_PROFILE_SLOT_A_ADDR 0x0006E000UL
#define NRFCLAW_LORA_PROFILE_SLOT_B_ADDR 0x0006F000UL

void nrfclaw_lora_profile_store_init(void);
void nrfclaw_lora_profile_store_process(void);
bool nrfclaw_lora_profile_store_load(nrfclaw_lora_profile_t *out);
bool nrfclaw_lora_profile_store_request_save(nrfclaw_lora_profile_t const *profile);
bool nrfclaw_lora_profile_store_busy(void);
uint32_t nrfclaw_lora_profile_store_generation(void);

#endif
