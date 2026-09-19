#include "nrfclaw_ninalink_auto_discovery.h"

#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_node_registry.h"
#include <string.h>

static bool m_enabled = true;
static bool m_inflight;
static uint8_t m_kind;
static uint32_t m_node_id;
static uint16_t m_request_seq;

static uint8_t sat_add_u8(uint8_t a, uint8_t b)
{
    uint16_t v = (uint16_t)a + (uint16_t)b;
    return v > 0xFFU ? 0xFFU : (uint8_t)v;
}

static uint8_t sat_inc_u8(uint8_t v)
{
    return v == 0xFFU ? v : (uint8_t)(v + 1U);
}

static void clear_inflight(void)
{
    m_inflight = false;
    m_kind = NRFCLAW_NINALINK_AUTO_KIND_NONE;
    m_node_id = 0U;
    m_request_seq = 0U;
}

static void set_error(uint32_t node_id, uint8_t error, bool terminal)
{
    nrfclaw_ninalink_node_discovery_t d;
    if (!nrfclaw_ninalink_node_registry_discovery_get(node_id, &d)) return;
    d.last_error = error;
    if (terminal)
        d.state = NRFCLAW_NINALINK_NODE_DISC_ERROR;
    else
        d.retry_count = sat_inc_u8(d.retry_count);
    (void)nrfclaw_ninalink_node_registry_discovery_set(node_id, &d);
}

void nrfclaw_ninalink_auto_discovery_reset_runtime(void)
{
    clear_inflight();
    m_enabled = true;
}

void nrfclaw_ninalink_auto_discovery_abort(void)
{
    bool cancelled = false;
    if (!m_inflight) return;
    if (m_kind == NRFCLAW_NINALINK_AUTO_KIND_CAPABILITY) {
        cancelled = nrfclaw_ninalink_bridge_cancel_capability_discovery(
            m_node_id, m_request_seq);
    } else if (m_kind == NRFCLAW_NINALINK_AUTO_KIND_COMMAND) {
        cancelled = nrfclaw_ninalink_bridge_cancel_command_discovery(
            m_node_id, m_request_seq);
    }
    if (cancelled) clear_inflight();
}

void nrfclaw_ninalink_auto_discovery_set_enabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) nrfclaw_ninalink_auto_discovery_abort();
}

void nrfclaw_ninalink_auto_discovery_get_status(
    nrfclaw_ninalink_auto_discovery_status_t *out)
{
    if (!out) return;
    out->enabled = m_enabled;
    out->inflight = m_inflight;
    out->kind = m_kind;
    out->node_id = m_node_id;
    out->request_seq = m_request_seq;
}

static void consume_capability_result(void)
{
    nrfclaw_ninalink_capability_discovery_status_t s;
    nrfclaw_ninalink_node_discovery_t d;

    nrfclaw_ninalink_bridge_get_capability_discovery_status(&s);
    if (s.target_node != m_node_id || s.request_seq != m_request_seq) return;

    if (s.pending) {
        if (s.timeout_count == 0U) return;
        if (nrfclaw_ninalink_bridge_cancel_capability_discovery(
                m_node_id, m_request_seq)) {
            set_error(m_node_id,
                      NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_TIMEOUT,
                      false);
            clear_inflight();
        }
        return;
    }

    if (s.state != NRFCLAW_NINALINK_APP_DL_DONE) return;
    if (!nrfclaw_ninalink_node_registry_discovery_get(m_node_id, &d)) {
        clear_inflight();
        return;
    }

    if (d.state != NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING ||
        s.page_index != d.next_capability_page) {
        set_error(m_node_id,
                  NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_PROTOCOL,
                  true);
        clear_inflight();
        return;
    }

    d.capability_registry_version = s.registry_version;
    d.capability_count = sat_add_u8(d.capability_count, s.count);
    d.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_NONE;

    if (s.more) {
        if (s.page_index == 0xFFU || s.count == 0U) {
            d.state = NRFCLAW_NINALINK_NODE_DISC_ERROR;
            d.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_PROTOCOL;
        } else {
            d.next_capability_page = (uint8_t)(s.page_index + 1U);
            d.state = NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING;
        }
    } else {
        d.state = NRFCLAW_NINALINK_NODE_DISC_CAPS_DONE;
    }

    (void)nrfclaw_ninalink_node_registry_discovery_set(m_node_id, &d);
    clear_inflight();
}

static void consume_command_result(void)
{
    nrfclaw_ninalink_command_discovery_status_t s;
    nrfclaw_ninalink_node_discovery_t d;
    uint16_t next;

    nrfclaw_ninalink_bridge_get_command_discovery_status(&s);
    if (s.target_node != m_node_id || s.request_seq != m_request_seq) return;

    if (s.pending) {
        if (s.timeout_count == 0U) return;
        if (nrfclaw_ninalink_bridge_cancel_command_discovery(
                m_node_id, m_request_seq)) {
            set_error(m_node_id,
                      NRFCLAW_NINALINK_NODE_DISC_ERR_COMMANDS_TIMEOUT,
                      false);
            clear_inflight();
        }
        return;
    }

    if (s.state != NRFCLAW_NINALINK_APP_DL_DONE) return;
    if (!nrfclaw_ninalink_node_registry_discovery_get(m_node_id, &d)) {
        clear_inflight();
        return;
    }

    if (d.state != NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING ||
        s.start_index != d.next_command_index) {
        set_error(m_node_id,
                  NRFCLAW_NINALINK_NODE_DISC_ERR_COMMANDS_PROTOCOL,
                  true);
        clear_inflight();
        return;
    }

    d.command_registry_version = s.registry_version;
    d.command_count = s.total_count;
    d.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_NONE;
    next = (uint16_t)s.start_index + (uint16_t)s.count;

    if (next < s.total_count) {
        if (s.count == 0U || next > 0xFFU) {
            d.state = NRFCLAW_NINALINK_NODE_DISC_ERROR;
            d.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_COMMANDS_PROTOCOL;
        } else {
            d.next_command_index = (uint8_t)next;
            d.state = NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING;
        }
    } else {
        d.state = NRFCLAW_NINALINK_NODE_DISC_READY;
    }

    (void)nrfclaw_ninalink_node_registry_discovery_set(m_node_id, &d);
    clear_inflight();
}

void nrfclaw_ninalink_auto_discovery_process(void)
{
    if (!m_inflight) return;
    if (m_kind == NRFCLAW_NINALINK_AUTO_KIND_CAPABILITY)
        consume_capability_result();
    else if (m_kind == NRFCLAW_NINALINK_AUTO_KIND_COMMAND)
        consume_command_result();
    else
        clear_inflight();
}

static bool queue_capability(
    uint32_t node_id,
    nrfclaw_ninalink_node_discovery_t *d)
{
    nrfclaw_ninalink_capability_discovery_status_t s;
    if (!nrfclaw_ninalink_bridge_queue_capability_discovery(
            node_id, d->next_capability_page))
        return false;
    nrfclaw_ninalink_bridge_get_capability_discovery_status(&s);
    m_inflight = true;
    m_kind = NRFCLAW_NINALINK_AUTO_KIND_CAPABILITY;
    m_node_id = node_id;
    m_request_seq = s.request_seq;
    return true;
}

static bool queue_commands(
    uint32_t node_id,
    nrfclaw_ninalink_node_discovery_t *d)
{
    nrfclaw_ninalink_command_discovery_status_t s;
    if (!nrfclaw_ninalink_bridge_queue_command_discovery(
            node_id, d->next_command_index))
        return false;
    nrfclaw_ninalink_bridge_get_command_discovery_status(&s);
    m_inflight = true;
    m_kind = NRFCLAW_NINALINK_AUTO_KIND_COMMAND;
    m_node_id = node_id;
    m_request_seq = s.request_seq;
    return true;
}

void nrfclaw_ninalink_auto_discovery_on_contact(uint32_t node_id)
{
    nrfclaw_ninalink_node_discovery_t d;
    nrfclaw_ninalink_node_discovery_t next;

    nrfclaw_ninalink_auto_discovery_process();

    if (!m_enabled || m_inflight)
        return;

    if (!nrfclaw_ninalink_node_registry_discovery_get(node_id, &d))
        return;

    if (d.state == NRFCLAW_NINALINK_NODE_DISC_NEW) {
        memset(&next, 0, sizeof(next));
        next.state = NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING;
        next.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_NONE;

        if (!queue_capability(node_id, &next))
            return;

        if (!nrfclaw_ninalink_node_registry_discovery_set(
                node_id, &next))
            nrfclaw_ninalink_auto_discovery_abort();
        return;
    }

    if (d.state == NRFCLAW_NINALINK_NODE_DISC_CAPS_DONE) {
        next = d;
        next.state =
            NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING;
        next.next_command_index = 0U;
        next.last_error = NRFCLAW_NINALINK_NODE_DISC_ERR_NONE;

        if (!queue_commands(node_id, &next))
            return;

        if (!nrfclaw_ninalink_node_registry_discovery_set(
                node_id, &next))
            nrfclaw_ninalink_auto_discovery_abort();
        return;
    }

    if (d.state == NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING)
        (void)queue_capability(node_id, &d);
    else if (d.state ==
             NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING)
        (void)queue_commands(node_id, &d);
}
