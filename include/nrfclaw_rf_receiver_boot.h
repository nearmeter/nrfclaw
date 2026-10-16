#ifndef NRFCLAW_RF_RECEIVER_BOOT_H
#define NRFCLAW_RF_RECEIVER_BOOT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_BRIDGE_RADIO_MODE_LORA = 0,
    NRFCLAW_BRIDGE_RADIO_MODE_EXTERNAL_RF = 1
} nrfclaw_bridge_radio_mode_t;

typedef struct {
    bool persisted;
    bool enabled;
    bool runtime_active;
    uint8_t profile_id;
    uint8_t bw_code;
    uint32_t frequency_hz;
    uint8_t store_status;
} nrfclaw_rf_receiver_boot_status_t;

void nrfclaw_rf_receiver_boot_init(void);

/* B8.0l canonical exclusive bridge-radio mode API. */
nrfclaw_bridge_radio_mode_t nrfclaw_bridge_radio_mode_get(void);
bool nrfclaw_bridge_radio_mode_set_lora_persist(void);
bool nrfclaw_bridge_radio_mode_set_external_persist(
    uint8_t profile_id,
    uint32_t frequency_hz,
    uint8_t bw_code);
bool nrfclaw_bridge_radio_mode_is_external(void);

void nrfclaw_rf_receiver_boot_note_runtime(uint8_t profile_id,
                                           uint32_t frequency_hz,
                                           uint8_t bw_code);
bool nrfclaw_rf_receiver_boot_start_saved(void);
bool nrfclaw_rf_receiver_boot_enable_persist(void);
bool nrfclaw_rf_receiver_boot_disable_persist(void);
bool nrfclaw_rf_receiver_boot_enabled(void);
void nrfclaw_rf_receiver_boot_get_status(
    nrfclaw_rf_receiver_boot_status_t *out);

#endif
