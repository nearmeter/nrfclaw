#include "nrfclaw_ninalink_external.h"
#include "nrfclaw_ninalink_node_registry.h"
#include "nrfclaw_ninalink_state_cache.h"
#include <string.h>

static uint16_t age16(uint32_t now_s, uint32_t then_s)
{
    uint32_t age;
    if (now_s < then_s) return 0U;
    age = now_s - then_s;
    return age > 0xFFFFU ? 0xFFFFU : (uint16_t)age;
}

void nrfclaw_ninalink_external_get_status(
    nrfclaw_ninalink_external_status_t *out)
{
    nrfclaw_ninalink_node_registry_status_t rs;
    nrfclaw_ninalink_state_cache_status_t ss;
    nrfclaw_ninalink_event_history_status_t es;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    nrfclaw_ninalink_node_registry_get_status(&rs);
    nrfclaw_ninalink_state_cache_get_status(&ss);
    nrfclaw_ninalink_event_history_get_status(&es);
    out->schema_version = NRFCLAW_NINALINK_EXTERNAL_SCHEMA_VERSION;
    out->node_count = rs.count;
    out->state_value_count = ss.values;
    out->event_count = es.entries;
    out->oldest_event_id = es.oldest_id;
    out->newest_event_id = es.newest_id;
}

bool nrfclaw_ninalink_external_get_node(
    uint8_t logical_index, uint32_t now_s,
    nrfclaw_ninalink_external_node_t *out)
{
    nrfclaw_ninalink_node_entry_t node;
    nrfclaw_ninalink_state_cache_node_t state;
    if (!out || !nrfclaw_ninalink_node_registry_get_at(logical_index, &node))
        return false;
    memset(out, 0, sizeof(*out));
    out->node_id = node.node_id;
    out->discovery_state = node.discovery.state;
    out->age_s = age16(now_s, node.last_seen_s);
    out->rssi_x2 = node.rssi_x2;
    out->snr_x4 = node.snr_x4;
    out->flags = NRFCLAW_NINALINK_EXTERNAL_NODE_VALID;
    out->last_message_type = node.last_message_type;
    if (node.discovery.state == NRFCLAW_NINALINK_NODE_DISC_READY)
        out->flags |= NRFCLAW_NINALINK_EXTERNAL_NODE_READY;
    out->event_count = nrfclaw_ninalink_event_history_count(node.node_id);
    if (nrfclaw_ninalink_state_cache_get_node(node.node_id, &state)) {
        out->state_count = state.value_count;
        if (state.value_count != 0U)
            out->flags |= NRFCLAW_NINALINK_EXTERNAL_NODE_HAS_STATE;
        if (state.session_valid)
            out->flags |= NRFCLAW_NINALINK_EXTERNAL_NODE_SESSION_VALID;
    }
    return true;
}

bool nrfclaw_ninalink_external_get_state(
    uint32_t node_id, uint8_t logical_index, uint32_t now_s,
    nrfclaw_ninalink_external_state_t *out)
{
    nrfclaw_ninalink_cached_value_t value;
    if (!out || !nrfclaw_ninalink_state_cache_get_value_at(
            node_id, logical_index, &value)) return false;
    memset(out, 0, sizeof(*out));
    out->capability_id = value.capability_id;
    out->channel = value.channel;
    out->value_type = value.value.type;
    out->raw_value = nrfclaw_ninalink_state_cache_value_raw(&value.value);
    out->sequence = value.last_sequence;
    out->age_s = age16(now_s, value.updated_s);
    out->update_count = value.update_count;
    out->source_message_type = value.last_message_type;
    return true;
}

bool nrfclaw_ninalink_external_get_event_after(
    uint32_t cursor, nrfclaw_ninalink_external_event_meta_t *out)
{
    nrfclaw_ninalink_cached_event_t event;
    if (!out || !nrfclaw_ninalink_event_history_get_after(cursor, &event))
        return false;
    memset(out, 0, sizeof(*out));
    out->event_id = event.event_id;
    out->node_id = event.node_id;
    out->capability_id = event.capability_id;
    out->channel = event.channel;
    out->value_type = event.value.type;
    out->sequence = event.sequence;
    return true;
}

bool nrfclaw_ninalink_external_get_event_value(
    uint32_t event_id, uint32_t now_s,
    nrfclaw_ninalink_external_event_value_t *out)
{
    nrfclaw_ninalink_cached_event_t event;
    if (!out || !nrfclaw_ninalink_event_history_get_by_id(event_id, &event))
        return false;
    memset(out, 0, sizeof(*out));
    out->raw_value = nrfclaw_ninalink_state_cache_value_raw(&event.value);
    out->age_s = age16(now_s, event.occurred_s);
    return true;
}

void nrfclaw_ninalink_external_get_descriptor(
    uint16_t capability_id, uint8_t channel,
    nrfclaw_ninalink_external_descriptor_t *out)
{
    nrfclaw_capability_desc_t desc;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* Never invent semantics for unknown/vendor/private IDs. */
    if (!nrfclaw_capability_descriptor(capability_id, channel, &desc)) return;
    out->known = true;
    out->kind = desc.kind;
    out->value_type = desc.value_type;
    out->scale10 = desc.scale10;
    out->unit = desc.unit;
    out->behavior_flags = desc.behavior_flags;
}
