#ifndef NRFCLAW_RF_PROFILE_REGISTRY_H
#define NRFCLAW_RF_PROFILE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_rfvm.h"

#define NRFCLAW_RF_PROFILE_NAME_MAX 24U
#define NRFCLAW_RF_PROFILE_SYNC_MAX 4U

typedef enum {
    NRFCLAW_RF_MODULATION_FSK_PCM = 1
} nrfclaw_rf_modulation_t;

enum {
    NRFCLAW_RF_PROFILE_FLAG_CATALOG            = 0x01U,
    NRFCLAW_RF_PROFILE_FLAG_RFVM_READY         = 0x02U,
    NRFCLAW_RF_PROFILE_FLAG_RADIO_READY        = 0x04U,
    NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY   = 0x08U,
    NRFCLAW_RF_PROFILE_FLAG_HARDWARE_VALIDATED = 0x10U,
    NRFCLAW_RF_PROFILE_FLAG_VARIABLE_PACKET   = 0x20U
};

typedef struct {
    uint8_t profile_id;
    const char *name;
    const char *source_decoder;
    uint8_t modulation;
    uint8_t flags;
    uint32_t frequency_hz;
    uint32_t bitrate_bps;
    uint32_t deviation_hz;
    uint32_t rx_bandwidth_hz;
    uint8_t rx_bw_code;
    uint16_t preamble_bits;
    uint8_t sync[NRFCLAW_RF_PROFILE_SYNC_MAX];
    uint8_t sync_len;
    uint8_t expected_len;
    const nrfclaw_rfvm_profile_t *rfvm;
} nrfclaw_rf_profile_entry_t;

uint8_t nrfclaw_rf_profile_registry_count(void);
const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_at(uint8_t index);
const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_get(uint8_t profile_id);
const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_find(const char *name);

bool nrfclaw_rf_profile_registry_rfvm_ready(uint8_t profile_id);
bool nrfclaw_rf_profile_registry_runtime_ready(uint8_t profile_id);
const nrfclaw_rfvm_profile_t *
nrfclaw_rf_profile_registry_rfvm(uint8_t profile_id);

#endif
