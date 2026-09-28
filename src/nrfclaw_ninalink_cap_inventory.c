#include "nrfclaw_ninalink_cap_inventory.h"

#include <string.h>

typedef struct {
    bool valid;
    uint32_t node_id;
    uint8_t registry_version;
    uint8_t count;
    bool complete;
    bool overflow;
    nrfclaw_ninalink_capability_descriptor_t
        descriptors[NRFCLAW_NINALINK_CAP_INVENTORY_PER_NODE];
} inventory_node_t;

static inventory_node_t m_nodes[NRFCLAW_NINALINK_CAP_INVENTORY_NODES];

static inventory_node_t *find_node(uint32_t node_id)
{
    uint8_t i;
    for (i = 0U; i < NRFCLAW_NINALINK_CAP_INVENTORY_NODES; i++) {
        if (m_nodes[i].valid && m_nodes[i].node_id == node_id)
            return &m_nodes[i];
    }
    return NULL;
}

static inventory_node_t *alloc_node(uint32_t node_id)
{
    uint8_t i;
    inventory_node_t *n = find_node(node_id);
    if (n)
        return n;

    for (i = 0U; i < NRFCLAW_NINALINK_CAP_INVENTORY_NODES; i++) {
        if (!m_nodes[i].valid) {
            memset(&m_nodes[i], 0, sizeof(m_nodes[i]));
            m_nodes[i].valid = true;
            m_nodes[i].node_id = node_id;
            return &m_nodes[i];
        }
    }
    return NULL;
}

static int find_descriptor(
    const inventory_node_t *n,
    uint16_t capability_id,
    uint8_t channel)
{
    uint8_t i;
    for (i = 0U; i < n->count; i++) {
        if (n->descriptors[i].desc.capability_id == capability_id &&
            n->descriptors[i].desc.channel == channel)
            return (int)i;
    }
    return -1;
}

void nrfclaw_ninalink_cap_inventory_clear(void)
{
    memset(m_nodes, 0, sizeof(m_nodes));
}

bool nrfclaw_ninalink_cap_inventory_store_page(
    uint32_t node_id,
    uint8_t registry_version,
    uint8_t page_index,
    const nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t count,
    bool more)
{
    inventory_node_t *n;
    uint8_t i;

    if (node_id == 0U || node_id == 0xFFFFFFFFUL)
        return false;
    if (count != 0U && descriptors == NULL)
        return false;

    n = find_node(node_id);

    if (page_index == 0U) {
        if (!n)
            n = alloc_node(node_id);
        if (!n)
            return false;

        memset(n, 0, sizeof(*n));
        n->valid = true;
        n->node_id = node_id;
        n->registry_version = registry_version;
    } else {
        if (!n || n->registry_version != registry_version)
            return false;
    }

    for (i = 0U; i < count; i++) {
        int at = find_descriptor(
            n,
            descriptors[i].desc.capability_id,
            descriptors[i].desc.channel);

        if (at >= 0) {
            n->descriptors[(uint8_t)at] = descriptors[i];
            continue;
        }

        if (n->count >= NRFCLAW_NINALINK_CAP_INVENTORY_PER_NODE) {
            n->overflow = true;
            continue;
        }

        n->descriptors[n->count++] = descriptors[i];
    }

    n->complete = !more;
    return true;
}

bool nrfclaw_ninalink_cap_inventory_get_status(
    uint32_t node_id,
    nrfclaw_ninalink_cap_inventory_status_t *out)
{
    inventory_node_t *n;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    n = find_node(node_id);
    if (!n)
        return false;

    out->valid = true;
    out->node_id = n->node_id;
    out->registry_version = n->registry_version;
    out->count = n->count;
    out->complete = n->complete;
    out->overflow = n->overflow;
    return true;
}

bool nrfclaw_ninalink_cap_inventory_get_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_capability_descriptor_t *out)
{
    inventory_node_t *n;

    if (!out)
        return false;

    n = find_node(node_id);
    if (!n || logical_index >= n->count)
        return false;

    *out = n->descriptors[logical_index];
    return true;
}
