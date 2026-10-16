#ifndef NRFCLAW_RF_NINALINK_H
#define NRFCLAW_RF_NINALINK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    uint8_t profile_id;
    uint32_t node_id;
    uint8_t capability_count;
    uint8_t cached_value_count;
} nrfclaw_rf_ninalink_status_t;

bool nrfclaw_rf_ninalink_publish(uint8_t profile_id, uint32_t now_s);
bool nrfclaw_rf_ninalink_status(nrfclaw_rf_ninalink_status_t *out);

#endif
