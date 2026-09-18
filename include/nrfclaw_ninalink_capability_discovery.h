#ifndef NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_H
#define NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_capability.h"
#include "nrfclaw_ninalink_msg.h"

#define NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX \
    NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS

typedef struct {
    uint8_t page_index;
    uint8_t count;
    bool more;
    nrfclaw_ninalink_capability_descriptor_t
        descriptors[NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX];
} nrfclaw_ninalink_capability_discovery_page_t;

uint16_t nrfclaw_ninalink_capability_supported_count(void);

bool nrfclaw_ninalink_capability_discovery_build_page(
    uint8_t page_index,
    nrfclaw_ninalink_capability_discovery_page_t *out);

#endif
