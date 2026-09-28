#ifndef NRFCLAW_NINALINK_CAP_INVENTORY_H
#define NRFCLAW_NINALINK_CAP_INVENTORY_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_ninalink_msg.h"

#define NRFCLAW_NINALINK_CAP_INVENTORY_NODES 16U
#define NRFCLAW_NINALINK_CAP_INVENTORY_PER_NODE 20U

typedef struct {
    bool valid;
    uint32_t node_id;
    uint8_t registry_version;
    uint8_t count;
    bool complete;
    bool overflow;
} nrfclaw_ninalink_cap_inventory_status_t;

void nrfclaw_ninalink_cap_inventory_clear(void);

bool nrfclaw_ninalink_cap_inventory_store_page(
    uint32_t node_id,
    uint8_t registry_version,
    uint8_t page_index,
    const nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t count,
    bool more);

bool nrfclaw_ninalink_cap_inventory_get_status(
    uint32_t node_id,
    nrfclaw_ninalink_cap_inventory_status_t *out);

bool nrfclaw_ninalink_cap_inventory_get_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_capability_descriptor_t *out);

#endif
