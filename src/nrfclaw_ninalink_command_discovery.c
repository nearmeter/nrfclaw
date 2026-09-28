#include "nrfclaw_ninalink_command_discovery.h"

#include <string.h>

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool nrfclaw_ninalink_command_discovery_build_page(
    uint8_t start_index,
    uint8_t *payload,
    uint8_t payload_capacity,
    uint8_t *payload_len)
{
    uint8_t total;
    uint8_t count;
    uint8_t i;
    uint8_t needed;

    if (!payload || !payload_len)
        return false;

    total = nrfclaw_ninalink_command_registry_count();

    if (start_index >= total)
        count = 0U;
    else {
        count = (uint8_t)(total - start_index);
        if (count > NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX)
            count = NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX;
    }

    needed = (uint8_t)(
        4U + count * NRFCLAW_NINALINK_COMMAND_DESCRIPTOR_WIRE_SIZE);

    if (payload_capacity < needed)
        return false;

    payload[0] = NRFCLAW_NINALINK_COMMAND_REGISTRY_VERSION;
    payload[1] = start_index;
    payload[2] = total;
    payload[3] = count;

    for (i = 0U; i < count; i++) {
        nrfclaw_ninalink_command_descriptor_t d;
        uint8_t *p = &payload[
            4U + i * NRFCLAW_NINALINK_COMMAND_DESCRIPTOR_WIRE_SIZE];

        if (!nrfclaw_ninalink_command_registry_get(
                (uint8_t)(start_index + i), &d))
            return false;

        put_u16_le(&p[0], d.command_id);
        p[2] = d.min_args;
        p[3] = d.max_args;
        p[4] = d.max_result;
        p[5] = d.flags;
    }

    *payload_len = needed;
    return true;
}

bool nrfclaw_ninalink_command_discovery_parse_page(
    const uint8_t *payload,
    uint8_t payload_len,
    nrfclaw_ninalink_command_discovery_page_t *out)
{
    uint8_t count;
    uint8_t expected;
    uint8_t i;

    if (!payload || !out || payload_len < 4U)
        return false;

    if (payload[0] != NRFCLAW_NINALINK_COMMAND_REGISTRY_VERSION)
        return false;

    count = payload[3];
    if (count > NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX)
        return false;

    expected = (uint8_t)(
        4U + count * NRFCLAW_NINALINK_COMMAND_DESCRIPTOR_WIRE_SIZE);
    if (payload_len != expected)
        return false;

    if (count && (uint16_t)payload[1] + count > payload[2])
        return false;

    memset(out, 0, sizeof(*out));
    out->registry_version = payload[0];
    out->start_index = payload[1];
    out->total_count = payload[2];
    out->count = count;

    for (i = 0U; i < count; i++) {
        const uint8_t *p = &payload[
            4U + i * NRFCLAW_NINALINK_COMMAND_DESCRIPTOR_WIRE_SIZE];
        nrfclaw_ninalink_command_descriptor_t *d = &out->descriptors[i];

        d->command_id = get_u16_le(&p[0]);
        d->min_args = p[2];
        d->max_args = p[3];
        d->max_result = p[4];
        d->flags = p[5];
    }

    return true;
}
