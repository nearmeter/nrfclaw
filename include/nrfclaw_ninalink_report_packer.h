#ifndef NRFCLAW_NINALINK_REPORT_PACKER_H
#define NRFCLAW_NINALINK_REPORT_PACKER_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink_msg.h"

typedef struct {
    uint16_t start_index;
    uint16_t next_index;
    uint16_t scanned;
    uint8_t packed;
    uint8_t deferred;
    uint8_t payload_used;
} nrfclaw_ninalink_report_pack_status_t;

void nrfclaw_ninalink_report_packer_reset(void);

bool nrfclaw_ninalink_report_pack(
    nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_capacity,
    uint8_t *entry_count,
    nrfclaw_ninalink_report_pack_status_t *status);

#endif
