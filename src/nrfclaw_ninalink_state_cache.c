#include "nrfclaw_ninalink_state_cache.h"
#include "nrfclaw_ninalink_command.h"
#include <string.h>

typedef struct {
    nrfclaw_ninalink_cached_value_t pub;
    uint32_t touch_order;
} value_slot_t;

typedef struct {
    nrfclaw_ninalink_state_cache_node_t pub;
    value_slot_t values[NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE];
    uint32_t touch_order;
    bool sequence_valid;
} node_slot_t;

static node_slot_t m_nodes[NRFCLAW_NINALINK_STATE_CACHE_NODES];
static uint32_t m_touch_order;
static uint16_t m_frames;
static uint16_t m_value_updates;
static uint16_t m_node_evictions;
static uint16_t m_value_evictions;

static nrfclaw_ninalink_cached_event_t
    m_event_history[NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH];
static uint8_t m_event_head;
static uint8_t m_event_count;
static uint16_t m_event_ingested;
static uint16_t m_event_dropped;
static uint32_t m_event_next_id;
static uint32_t m_state_revision;
static uint32_t m_event_revision;
static uint32_t m_change_revision;

static uint16_t sat_inc16(uint16_t v)
{
    return v == 0xFFFFU ? v : (uint16_t)(v + 1U);
}

static uint32_t revision_next(uint32_t value)
{
    value++;
    return value == 0U ? 1U : value;
}

static void mark_state_changed(void)
{
    m_state_revision = revision_next(m_state_revision);
    m_change_revision = revision_next(m_change_revision);
}

static void mark_event_changed(void)
{
    m_event_revision = revision_next(m_event_revision);
    m_change_revision = revision_next(m_change_revision);
}


static bool sequence_newer(uint16_t candidate, uint16_t reference)
{
    uint16_t diff = (uint16_t)(candidate - reference);
    return diff != 0U && diff < 0x8000U;
}

static uint32_t next_touch(void)
{
    uint8_t i;
    uint8_t j;
    m_touch_order++;
    if (m_touch_order != 0U)
        return m_touch_order;
    m_touch_order = 1U;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_NODES; i++) {
        if (!m_nodes[i].pub.valid)
            continue;
        m_nodes[i].touch_order = m_touch_order++;
        for (j = 0U; j < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; j++) {
            if (m_nodes[i].values[j].pub.valid)
                m_nodes[i].values[j].touch_order = m_touch_order++;
        }
    }
    return m_touch_order++;
}

static int find_node(uint32_t node_id)
{
    uint8_t i;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_NODES; i++) {
        if (m_nodes[i].pub.valid && m_nodes[i].pub.node_id == node_id)
            return (int)i;
    }
    return -1;
}

static uint8_t choose_node(void)
{
    uint8_t i;
    uint8_t oldest = 0U;
    uint32_t oldest_touch = 0xFFFFFFFFUL;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_NODES; i++) {
        if (!m_nodes[i].pub.valid)
            return i;
        if (m_nodes[i].touch_order < oldest_touch) {
            oldest_touch = m_nodes[i].touch_order;
            oldest = i;
        }
    }
    m_node_evictions = sat_inc16(m_node_evictions);
    return oldest;
}

static int find_value(const node_slot_t *node,
                      uint16_t capability_id,
                      uint8_t channel)
{
    uint8_t i;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; i++) {
        if (node->values[i].pub.valid &&
            node->values[i].pub.capability_id == capability_id &&
            node->values[i].pub.channel == channel)
            return (int)i;
    }
    return -1;
}

static uint8_t choose_value(node_slot_t *node)
{
    uint8_t i;
    uint8_t oldest = 0U;
    uint32_t oldest_touch = 0xFFFFFFFFUL;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; i++) {
        if (!node->values[i].pub.valid)
            return i;
        if (node->values[i].touch_order < oldest_touch) {
            oldest_touch = node->values[i].touch_order;
            oldest = i;
        }
    }
    m_value_evictions = sat_inc16(m_value_evictions);
    return oldest;
}

static void push_event(uint32_t node_id,
                       uint16_t sequence,
                       const nrfclaw_ninalink_value_entry_t *entry,
                       uint32_t now_s)
{
    nrfclaw_ninalink_cached_event_t *dst;

    if (!entry)
        return;

    if (m_event_count >= NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH)
        m_event_dropped = sat_inc16(m_event_dropped);
    else
        m_event_count++;

    dst = &m_event_history[m_event_head];
    memset(dst, 0, sizeof(*dst));
    dst->valid = true;
    dst->node_id = node_id;
    m_event_next_id++;
    if (m_event_next_id == 0U)
        m_event_next_id = 1U;
    dst->event_id = m_event_next_id;
    dst->capability_id = entry->capability_id;
    dst->channel = entry->channel;
    dst->value = entry->value;
    dst->sequence = sequence;
    dst->occurred_s = now_s;

    m_event_head = (uint8_t)((m_event_head + 1U) %
                             NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    m_event_ingested = sat_inc16(m_event_ingested);
}

void nrfclaw_ninalink_state_cache_clear(void)
{
    bool had_state = false;
    bool had_events = m_event_count != 0U;
    uint8_t i;

    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_NODES; i++) {
        if (m_nodes[i].pub.valid && m_nodes[i].pub.value_count != 0U) {
            had_state = true;
            break;
        }
    }
    memset(m_nodes, 0, sizeof(m_nodes));
    memset(m_event_history, 0, sizeof(m_event_history));
    m_touch_order = 0U;
    m_frames = 0U;
    m_value_updates = 0U;
    m_node_evictions = 0U;
    m_value_evictions = 0U;
    m_event_head = 0U;
    m_event_count = 0U;
    m_event_ingested = 0U;
    m_event_dropped = 0U;

    if (had_state)
        mark_state_changed();
    if (had_events)
        mark_event_changed();
}

bool nrfclaw_ninalink_state_cache_ingest_values_session(
    uint32_t node_id,
    uint32_t session_id,
    uint16_t sequence,
    uint8_t message_type,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count,
    uint32_t now_s)
{
    node_slot_t *node;
    int found;
    uint8_t node_index;
    uint8_t i;
    bool state_changed = false;

    if (node_id == 0U || node_id == 0xFFFFFFFFUL || !entries || entry_count == 0U)
        return false;
    if (message_type != NRFCLAW_NINALINK_MSG_CAP_REPORT &&
        message_type != NRFCLAW_NINALINK_MSG_CAP_EVENT)
        return false;

    found = find_node(node_id);
    if (found >= 0) {
        node_index = (uint8_t)found;
    } else {
        node_index = choose_node();
        memset(&m_nodes[node_index], 0, sizeof(m_nodes[node_index]));
        m_nodes[node_index].pub.valid = true;
        m_nodes[node_index].pub.node_id = node_id;
    }

    node = &m_nodes[node_index];

    if (session_id != 0U && session_id != 0xFFFFFFFFUL) {
        if (!node->pub.session_valid) {
            /* First session observation establishes an explicit epoch. */
            if (node->pub.updates != 0U) {
                if (node->pub.value_count != 0U)
                    state_changed = true;
                memset(node->values, 0, sizeof(node->values));
                node->pub.value_count = 0U;
            }
            node->pub.session_valid = true;
            node->pub.session_id = session_id;
            node->sequence_valid = false;
        } else if (node->pub.session_id != session_id) {
            /* A real node restart/session transition invalidates old state. */
            if (node->pub.value_count != 0U)
                state_changed = true;
            memset(node->values, 0, sizeof(node->values));
            node->pub.value_count = 0U;
            node->pub.session_id = session_id;
            node->pub.session_changes = sat_inc16(node->pub.session_changes);
            node->sequence_valid = false;
        }
    }

    if (!node->sequence_valid ||
        sequence_newer(sequence, node->pub.last_sequence)) {
        node->pub.last_sequence = sequence;
        node->pub.last_message_type = message_type;
        node->sequence_valid = true;
    }
    node->pub.last_seen_s = now_s;
    node->touch_order = next_touch();
    node->pub.updates = sat_inc16(node->pub.updates);
    if (message_type == NRFCLAW_NINALINK_MSG_CAP_REPORT)
        node->pub.reports = sat_inc16(node->pub.reports);
    else
        node->pub.events = sat_inc16(node->pub.events);
    m_frames = sat_inc16(m_frames);

    if (message_type == NRFCLAW_NINALINK_MSG_CAP_EVENT) {
        for (i = 0U; i < entry_count; i++)
            push_event(node_id, sequence, &entries[i], now_s);
        mark_event_changed();
        return true;
    }

    for (i = 0U; i < entry_count; i++) {
        value_slot_t *slot;
        uint8_t value_index;
        uint16_t old_count = 0U;
        found = find_value(node, entries[i].capability_id, entries[i].channel);
        if (found >= 0) {
            value_index = (uint8_t)found;
            if (!sequence_newer(
                    sequence,
                    node->values[value_index].pub.last_sequence))
                continue;
            old_count = node->values[value_index].pub.update_count;
        } else {
            value_index = choose_value(node);
        }
        slot = &node->values[value_index];
        state_changed = true;
        memset(&slot->pub, 0, sizeof(slot->pub));
        slot->pub.valid = true;
        slot->pub.capability_id = entries[i].capability_id;
        slot->pub.channel = entries[i].channel;
        slot->pub.value = entries[i].value;
        slot->pub.last_sequence = sequence;
        slot->pub.last_message_type = message_type;
        slot->pub.updated_s = now_s;
        slot->pub.update_count = old_count == 0xFFFFU ? old_count : (uint16_t)(old_count + 1U);
        if (slot->pub.update_count == 0U)
            slot->pub.update_count = 1U;
        slot->touch_order = next_touch();
        m_value_updates = sat_inc16(m_value_updates);
    }

    node->pub.value_count = 0U;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; i++) {
        if (node->values[i].pub.valid)
            node->pub.value_count++;
    }
    if (state_changed)
        mark_state_changed();

    return true;
}

bool nrfclaw_ninalink_state_cache_ingest_values(
    uint32_t node_id,
    uint16_t sequence,
    uint8_t message_type,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count,
    uint32_t now_s)
{
    return nrfclaw_ninalink_state_cache_ingest_values_session(
        node_id, 0U, sequence, message_type, entries, entry_count, now_s);
}

bool nrfclaw_ninalink_state_cache_upsert_query_values(
    uint32_t node_id,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count,
    uint32_t now_s)
{
    node_slot_t *node;
    int found;
    uint8_t node_index, i;

    if (node_id == 0U || node_id == 0xFFFFFFFFUL ||
        !entries || entry_count == 0U)
        return false;

    found = find_node(node_id);
    if (found >= 0) {
        node_index = (uint8_t)found;
    } else {
        node_index = choose_node();
        memset(&m_nodes[node_index], 0, sizeof(m_nodes[node_index]));
        m_nodes[node_index].pub.valid = true;
        m_nodes[node_index].pub.node_id = node_id;
    }

    node = &m_nodes[node_index];
    node->pub.last_seen_s = now_s;
    node->touch_order = next_touch();

    for (i = 0U; i < entry_count; i++) {
        value_slot_t *slot;
        uint8_t value_index;
        uint16_t old_count = 0U;
        found = find_value(node, entries[i].capability_id, entries[i].channel);
        if (found >= 0) {
            value_index = (uint8_t)found;
            old_count = node->values[value_index].pub.update_count;
        } else {
            value_index = choose_value(node);
        }
        slot = &node->values[value_index];
        memset(&slot->pub, 0, sizeof(slot->pub));
        slot->pub.valid = true;
        slot->pub.capability_id = entries[i].capability_id;
        slot->pub.channel = entries[i].channel;
        slot->pub.value = entries[i].value;
        slot->pub.last_sequence = node->pub.last_sequence;
        slot->pub.last_message_type = NRFCLAW_NINALINK_MSG_COMMAND_RESULT;
        slot->pub.updated_s = now_s;
        slot->pub.update_count = old_count == 0xFFFFU ? old_count : (uint16_t)(old_count + 1U);
        if (slot->pub.update_count == 0U) slot->pub.update_count = 1U;
        slot->touch_order = next_touch();
        m_value_updates = sat_inc16(m_value_updates);
    }

    node->pub.value_count = 0U;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; i++)
        if (node->values[i].pub.valid) node->pub.value_count++;

    mark_state_changed();
    return true;
}

void nrfclaw_ninalink_state_cache_get_status(
    nrfclaw_ninalink_state_cache_status_t *out)
{
    uint8_t i;
    uint8_t j;
    uint8_t nodes = 0U;
    uint8_t values = 0U;
    if (!out)
        return;
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_NODES; i++) {
        if (!m_nodes[i].pub.valid)
            continue;
        nodes++;
        for (j = 0U; j < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; j++) {
            if (m_nodes[i].values[j].pub.valid)
                values++;
        }
    }
    out->nodes = nodes;
    out->values = values;
    out->frames = m_frames;
    out->value_updates = m_value_updates;
    out->node_evictions = m_node_evictions;
    out->value_evictions = m_value_evictions;
}

bool nrfclaw_ninalink_state_cache_get_node(
    uint32_t node_id,
    nrfclaw_ninalink_state_cache_node_t *out)
{
    int found;
    if (!out)
        return false;
    found = find_node(node_id);
    if (found < 0)
        return false;
    *out = m_nodes[(uint8_t)found].pub;
    return true;
}

bool nrfclaw_ninalink_state_cache_get_value_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_cached_value_t *out)
{
    int found;
    uint8_t i;
    uint8_t logical = 0U;
    const node_slot_t *node;
    if (!out)
        return false;
    found = find_node(node_id);
    if (found < 0)
        return false;
    node = &m_nodes[(uint8_t)found];
    for (i = 0U; i < NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE; i++) {
        if (!node->values[i].pub.valid)
            continue;
        if (logical == logical_index) {
            *out = node->values[i].pub;
            return true;
        }
        logical++;
    }
    return false;
}

void nrfclaw_ninalink_event_history_get_status(
    nrfclaw_ninalink_event_history_status_t *out)
{
    uint8_t oldest;
    uint8_t newest;
    if (!out) return;
    out->entries = m_event_count;
    out->ingested = m_event_ingested;
    out->dropped = m_event_dropped;
    out->oldest_id = 0U;
    out->newest_id = 0U;
    if (m_event_count == 0U) return;
    oldest = (uint8_t)((m_event_head +
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH - m_event_count) %
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    newest = (uint8_t)((m_event_head +
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH - 1U) %
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    if (m_event_history[oldest].valid)
        out->oldest_id = m_event_history[oldest].event_id;
    if (m_event_history[newest].valid)
        out->newest_id = m_event_history[newest].event_id;
}

uint8_t nrfclaw_ninalink_event_history_count(uint32_t node_id)
{
    uint8_t i;
    uint8_t count = 0U;
    uint8_t oldest;

    if (node_id == 0U || node_id == 0xFFFFFFFFUL)
        return 0U;
    oldest = (uint8_t)((m_event_head +
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH - m_event_count) %
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    for (i = 0U; i < m_event_count; i++) {
        uint8_t pos = (uint8_t)((oldest + i) %
                      NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
        if (m_event_history[pos].valid &&
            m_event_history[pos].node_id == node_id)
            count++;
    }
    return count;
}

bool nrfclaw_ninalink_event_history_get_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_cached_event_t *out)
{
    uint8_t i;
    uint8_t seen = 0U;
    uint8_t oldest;

    if (!out)
        return false;
    oldest = (uint8_t)((m_event_head +
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH - m_event_count) %
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    for (i = 0U; i < m_event_count; i++) {
        uint8_t pos = (uint8_t)((oldest + i) %
                      NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
        const nrfclaw_ninalink_cached_event_t *event =
            &m_event_history[pos];
        if (!event->valid || event->node_id != node_id)
            continue;
        if (seen == logical_index) {
            *out = *event;
            return true;
        }
        seen++;
    }
    return false;
}

bool nrfclaw_ninalink_event_history_get_global_at(
    uint8_t logical_index,
    nrfclaw_ninalink_cached_event_t *out)
{
    uint8_t oldest;
    uint8_t pos;
    if (!out || logical_index >= m_event_count) return false;
    oldest = (uint8_t)((m_event_head +
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH - m_event_count) %
              NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    pos = (uint8_t)((oldest + logical_index) %
                    NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH);
    if (!m_event_history[pos].valid) return false;
    *out = m_event_history[pos];
    return true;
}

bool nrfclaw_ninalink_event_history_get_by_id(
    uint32_t event_id,
    nrfclaw_ninalink_cached_event_t *out)
{
    uint8_t i;
    nrfclaw_ninalink_cached_event_t event;
    if (!out || event_id == 0U) return false;
    for (i = 0U; i < m_event_count; i++) {
        if (!nrfclaw_ninalink_event_history_get_global_at(i, &event)) continue;
        if (event.event_id == event_id) { *out = event; return true; }
    }
    return false;
}

bool nrfclaw_ninalink_event_history_get_after(
    uint32_t cursor,
    nrfclaw_ninalink_cached_event_t *out)
{
    uint8_t i;
    nrfclaw_ninalink_cached_event_t event;
    if (!out) return false;
    for (i = 0U; i < m_event_count; i++) {
        if (!nrfclaw_ninalink_event_history_get_global_at(i, &event)) continue;
        if (event.event_id > cursor) { *out = event; return true; }
    }
    return false;
}

void nrfclaw_ninalink_state_cache_get_revisions(
    uint32_t *state_revision,
    uint32_t *event_revision,
    uint32_t *change_revision)
{
    if (state_revision)
        *state_revision = m_state_revision;
    if (event_revision)
        *event_revision = m_event_revision;
    if (change_revision)
        *change_revision = m_change_revision;
}

uint32_t nrfclaw_ninalink_state_cache_value_raw(
    const nrfclaw_capability_value_t *value)
{
    if (!value)
        return 0U;
    switch (value->type) {
        case NRFCLAW_CAP_VALUE_BOOL: return value->v.boolean ? 1U : 0U;
        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_ENUM8: return value->v.u8;
        case NRFCLAW_CAP_VALUE_S8: return (uint32_t)(int32_t)value->v.s8;
        case NRFCLAW_CAP_VALUE_U16: return value->v.u16;
        case NRFCLAW_CAP_VALUE_S16: return (uint32_t)(int32_t)value->v.s16;
        case NRFCLAW_CAP_VALUE_U32: return value->v.u32;
        case NRFCLAW_CAP_VALUE_S32: return (uint32_t)value->v.s32;
        default: return 0U;
    }
}
