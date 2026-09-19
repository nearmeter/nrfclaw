#include "nrfclaw_ninalink_node_registry.h"

#include <string.h>

typedef struct {
    nrfclaw_ninalink_node_entry_t pub;
    uint32_t touch_order;
} registry_slot_t;

static registry_slot_t m_slots[NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY];
static uint8_t m_count;
static uint16_t m_updates;
static uint16_t m_creations;
static uint16_t m_evictions;
static uint32_t m_touch_order;

static uint16_t sat_inc_u16(uint16_t v)
{
    return v == 0xFFFFU ? v : (uint16_t)(v + 1U);
}

static int find_slot(uint32_t node_id)
{
    uint8_t i;
    for (i = 0U; i < NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY; i++) {
        if (m_slots[i].pub.valid && m_slots[i].pub.node_id == node_id)
            return (int)i;
    }
    return -1;
}

static uint8_t choose_slot(void)
{
    uint8_t i;
    uint8_t oldest = 0U;
    uint32_t oldest_touch = 0xFFFFFFFFUL;

    for (i = 0U; i < NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY; i++) {
        if (!m_slots[i].pub.valid)
            return i;
        if (m_slots[i].touch_order < oldest_touch) {
            oldest_touch = m_slots[i].touch_order;
            oldest = i;
        }
    }
    m_evictions = sat_inc_u16(m_evictions);
    return oldest;
}

void nrfclaw_ninalink_node_registry_clear(void)
{
    memset(m_slots, 0, sizeof(m_slots));
    m_count = 0U;
    m_updates = 0U;
    m_creations = 0U;
    m_evictions = 0U;
    m_touch_order = 0U;
}

void nrfclaw_ninalink_node_registry_observe(
    const nrfclaw_ninalink_frame_t *frame,
    int16_t rssi_x2,
    int16_t snr_x4,
    uint32_t now_s)
{
    registry_slot_t *slot;
    int found;
    uint8_t index;

    if (!frame || frame->node_id == 0U ||
        frame->node_id == NRFCLAW_NINALINK_NODE_BROADCAST)
        return;

    found = find_slot(frame->node_id);
    if (found >= 0) {
        index = (uint8_t)found;
    } else {
        index = choose_slot();
        if (!m_slots[index].pub.valid &&
            m_count < NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY)
            m_count++;
        memset(&m_slots[index], 0, sizeof(m_slots[index]));
        m_slots[index].pub.valid = true;
        m_slots[index].pub.node_id = frame->node_id;
        m_slots[index].pub.discovery.state = NRFCLAW_NINALINK_NODE_DISC_NEW;
        m_creations = sat_inc_u16(m_creations);
    }

    slot = &m_slots[index];
    slot->pub.network_id = frame->network_id;
    slot->pub.last_sequence = frame->sequence;
    slot->pub.last_message_type = frame->message_type;
    slot->pub.last_seen_s = now_s;
    slot->pub.rssi_x2 = rssi_x2;
    slot->pub.snr_x4 = snr_x4;
    slot->pub.seen_count = sat_inc_u16(slot->pub.seen_count);
    m_updates = sat_inc_u16(m_updates);

    m_touch_order++;
    if (m_touch_order == 0U) {
        uint8_t i;
        uint32_t next = 1U;
        for (i = 0U; i < NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY; i++) {
            if (m_slots[i].pub.valid)
                m_slots[i].touch_order = next++;
        }
        m_touch_order = next;
    }
    slot->touch_order = m_touch_order;
}

void nrfclaw_ninalink_node_registry_get_status(
    nrfclaw_ninalink_node_registry_status_t *out)
{
    if (!out) return;
    out->count = m_count;
    out->capacity = NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY;
    out->updates = m_updates;
    out->creations = m_creations;
    out->evictions = m_evictions;
}

bool nrfclaw_ninalink_node_registry_get_at(
    uint8_t logical_index,
    nrfclaw_ninalink_node_entry_t *out)
{
    uint8_t i;
    uint8_t logical = 0U;
    if (!out || logical_index >= m_count) return false;
    for (i = 0U; i < NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY; i++) {
        if (!m_slots[i].pub.valid) continue;
        if (logical == logical_index) {
            *out = m_slots[i].pub;
            return true;
        }
        logical++;
    }
    return false;
}

bool nrfclaw_ninalink_node_registry_find(
    uint32_t node_id,
    nrfclaw_ninalink_node_entry_t *out)
{
    int found;
    if (!out) return false;
    found = find_slot(node_id);
    if (found < 0) return false;
    *out = m_slots[(uint8_t)found].pub;
    return true;
}

bool nrfclaw_ninalink_node_registry_discovery_get(
    uint32_t node_id,
    nrfclaw_ninalink_node_discovery_t *out)
{
    int found;
    if (!out) return false;
    found = find_slot(node_id);
    if (found < 0) return false;
    *out = m_slots[(uint8_t)found].pub.discovery;
    return true;
}

bool nrfclaw_ninalink_node_registry_discovery_set(
    uint32_t node_id,
    const nrfclaw_ninalink_node_discovery_t *value)
{
    int found;
    if (!value) return false;
    found = find_slot(node_id);
    if (found < 0) return false;
    m_slots[(uint8_t)found].pub.discovery = *value;
    return true;
}
