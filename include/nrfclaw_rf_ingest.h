#ifndef NRFCLAW_RF_INGEST_H
#define NRFCLAW_RF_INGEST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    uint8_t profile_id;
    uint16_t dio1_events;
    uint16_t packets;
    uint16_t decode_ok;
    uint16_t publish_ok;
    uint16_t failures;
    uint16_t last_irq;
    uint8_t last_len;
} nrfclaw_rf_ingest_status_t;

bool nrfclaw_rf_ingest_start(uint8_t profile_id);
void nrfclaw_rf_ingest_stop(void);
bool nrfclaw_rf_ingest_active(void);
bool nrfclaw_rf_ingest_on_dio1(void);
bool nrfclaw_rf_ingest_last_frame_meta(
    uint8_t *profile_id, uint8_t *length, bool *rfvm_ok);
uint8_t nrfclaw_rf_ingest_last_frame_read(
    uint8_t offset, uint8_t *out, uint8_t max_len);

void nrfclaw_rf_ingest_get_status(nrfclaw_rf_ingest_status_t *out);
void nrfclaw_rf_ingest_clear_stats(void);

#endif
