#include "nrfclaw_ninalink_report_packer.h"

#include "nrfclaw_capability.h"
#include "nrfclaw_ninalink.h"

#include <string.h>

#define REPORT_SESSION_VALUE_BYTES 4U
#define REPORT_SESSION_ENTRY_BYTES \
    (NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE + REPORT_SESSION_VALUE_BYTES)

#define REPORT_APP_PAYLOAD_LIMIT \
    (NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE - REPORT_SESSION_ENTRY_BYTES)

static uint16_t m_report_cursor;

static bool runtime_reportable(
    const nrfclaw_capability_desc_t *desc,
    uint8_t state_flags)
{
    const uint8_t required =
        NRFCLAW_CAP_STATE_SUPPORTED |
        NRFCLAW_CAP_STATE_PRESENT |
        NRFCLAW_CAP_STATE_ENABLED;

    if (!desc)
        return false;

    if ((desc->behavior_flags & NRFCLAW_CAP_BEHAVIOR_REPORTABLE) == 0U)
        return false;

    if (desc->kind == NRFCLAW_CAP_KIND_EVENT)
        return false;

    if ((state_flags & required) != required)
        return false;

    if ((state_flags & NRFCLAW_CAP_STATE_FAULT) != 0U)
        return false;

    return true;
}

void nrfclaw_ninalink_report_packer_reset(void)
{
    m_report_cursor = 0U;
}

bool nrfclaw_ninalink_report_pack(
    nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_capacity,
    uint8_t *entry_count,
    nrfclaw_ninalink_report_pack_status_t *status)
{
    const uint16_t registry_count = nrfclaw_capability_registry_count();
    uint16_t start;
    uint16_t step;
    uint16_t first_deferred = 0xFFFFU;
    uint8_t count = 0U;
    uint8_t used = 1U;
    uint8_t deferred = 0U;

    if (!entries || !entry_count || entry_capacity == 0U)
        return false;

    *entry_count = 0U;
    memset(entries, 0, (size_t)entry_capacity * sizeof(entries[0]));

    if (status)
        memset(status, 0, sizeof(*status));

    if (registry_count == 0U)
        return false;

    start = (uint16_t)(m_report_cursor % registry_count);

    for (step = 0U; step < registry_count; step++) {
        const uint16_t index =
            (uint16_t)((start + step) % registry_count);
        nrfclaw_capability_desc_t desc;
        nrfclaw_capability_value_t value;
        uint8_t state_flags = 0U;
        uint8_t value_size;
        uint8_t entry_size;

        if (!nrfclaw_capability_registry_at(index, &desc))
            continue;

        if ((desc.behavior_flags &
             NRFCLAW_CAP_BEHAVIOR_REPORTABLE) == 0U)
            continue;

        if (desc.kind == NRFCLAW_CAP_KIND_EVENT)
            continue;

        if (!nrfclaw_capability_state(
                desc.capability_id, desc.channel, &state_flags))
            continue;

        if (!runtime_reportable(&desc, state_flags))
            continue;

        if (!nrfclaw_capability_read_current(
                desc.capability_id, desc.channel, &value))
            continue;

        if (value.type != desc.value_type)
            continue;

        value_size = nrfclaw_capability_value_size(value.type);
        if (value_size == 0U)
            continue;

        entry_size = (uint8_t)(
            NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE + value_size);

        if (count >= entry_capacity ||
            (uint16_t)used + entry_size > REPORT_APP_PAYLOAD_LIMIT) {
            if (first_deferred == 0xFFFFU)
                first_deferred = index;
            if (deferred != 0xFFU)
                deferred++;
            continue;
        }

        entries[count].capability_id = desc.capability_id;
        entries[count].channel = desc.channel;
        entries[count].value = value;
        count++;
        used = (uint8_t)(used + entry_size);
    }

    if (first_deferred != 0xFFFFU)
        m_report_cursor = first_deferred;

    *entry_count = count;

    if (status) {
        status->start_index = start;
        status->next_index = m_report_cursor;
        status->scanned = registry_count;
        status->packed = count;
        status->deferred = deferred;
        status->payload_used = used;
    }

    return count != 0U;
}
