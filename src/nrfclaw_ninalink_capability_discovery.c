#include "nrfclaw_ninalink_capability_discovery.h"

#include <string.h>

uint16_t nrfclaw_ninalink_capability_supported_count(void)
{
    uint16_t registry_count;
    uint16_t i;
    uint16_t supported = 0U;

    registry_count = nrfclaw_capability_registry_count();

    for (i = 0U; i < registry_count; i++) {
        nrfclaw_capability_desc_t desc;
        uint8_t state = 0U;

        if (!nrfclaw_capability_registry_at(i, &desc))
            continue;

        if (!nrfclaw_capability_state(
                desc.capability_id,
                desc.channel,
                &state))
            continue;

        if ((state & NRFCLAW_CAP_STATE_SUPPORTED) != 0U)
            supported++;
    }

    return supported;
}

bool nrfclaw_ninalink_capability_discovery_build_page(
    uint8_t page_index,
    nrfclaw_ninalink_capability_discovery_page_t *out)
{
    uint16_t registry_count;
    uint16_t supported_total;
    uint16_t logical_index = 0U;
    uint16_t start_index;
    uint16_t i;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    out->page_index = page_index;

    supported_total = nrfclaw_ninalink_capability_supported_count();
    start_index = (uint16_t)page_index *
                  NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX;

    registry_count = nrfclaw_capability_registry_count();

    for (i = 0U; i < registry_count; i++) {
        nrfclaw_capability_desc_t desc;
        uint8_t state = 0U;

        if (!nrfclaw_capability_registry_at(i, &desc))
            continue;

        if (!nrfclaw_capability_state(
                desc.capability_id,
                desc.channel,
                &state))
            continue;

        if ((state & NRFCLAW_CAP_STATE_SUPPORTED) == 0U)
            continue;

        if (logical_index >= start_index &&
            out->count < NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX) {
            nrfclaw_ninalink_capability_descriptor_t *dst =
                &out->descriptors[out->count];

            memset(dst, 0, sizeof(*dst));
            dst->desc = desc;
            dst->runtime_state_flags = state;
            out->count++;
        }

        logical_index++;
    }

    out->more =
        ((uint16_t)start_index + out->count) < supported_total;

    return true;
}
