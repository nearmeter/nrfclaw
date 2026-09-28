#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_node_registry.h"
#include "nrfclaw_ninalink_cap_inventory.h"
#include "nrfclaw_ninalink_auto_discovery.h"
#include "nrfclaw_ninalink_state_cache.h"
#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_capability.h"
#include "nrfclaw_ninalink_network.h"
#include "nrfclaw_ninalink_query.h"

#include "app_timer.h"
#include "nrf.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_ninalink_msg.h"
#include "nrfclaw_ninalink_command.h"
#include "nrfclaw_ninalink_command_discovery.h"
#include "nrfclaw_ninalink_capability_discovery.h"

#include <string.h>

#define BRIDGE_TX_DELAY_MS       60U
#define BRIDGE_APP_RESULT_RX_MS 600U
#define BRIDGE_ACK_FRAME_LEN     18U
#define BRIDGE_CAP_SET_LEN       22U


#define COMMAND_PAYLOAD_BASE     5U
#define COMMAND_RESULT_BASE      4U

#define BRIDGE_SESSION_NODES 16U

typedef struct {
    uint8_t len;
    uint8_t data[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    int16_t rssi_x2;
    int16_t snr_x4;
} bridge_queue_packet_t;

typedef struct {
    bool valid;
    uint32_t node_id;
    uint32_t session_id;
} bridge_session_entry_t;

typedef struct {
    bool valid;
    uint16_t network_id;
    uint32_t node_id;
    uint16_t sequence;
    uint8_t message_type;
} bridge_dup_entry_t;

typedef enum {
    BRIDGE_TX_IDLE = 0,
    BRIDGE_TX_DELAY,
    BRIDGE_TX_WAIT,
    BRIDGE_TX_WAIT_APP_RESULT
} bridge_tx_state_t;

typedef enum {
    BRIDGE_TX_KIND_NONE = 0,
    BRIDGE_TX_KIND_ACK,
    BRIDGE_TX_KIND_CAP_SET,
    BRIDGE_TX_KIND_COMMAND,
    BRIDGE_TX_KIND_COMMAND_DISCOVERY,
    BRIDGE_TX_KIND_CAPABILITY_DISCOVERY
} bridge_tx_kind_t;

APP_TIMER_DEF(m_bridge_tx_timer);

static bool m_active;
static bool m_timer_initialized;
static volatile bool m_tx_due;
static bridge_tx_state_t m_tx_state;
static bridge_tx_kind_t m_tx_kind;

/*
 * B4.8 recovery guard.
 *
 * A timed RX completion can leave the LLCC68 busy for a short interval after
 * nrfclaw_lora_take_rx(). Do not permanently disable the bridge if the
 * immediate continuous-RX rearm races that cleanup.
 */
static bool m_restart_pending;

static bridge_queue_packet_t m_queue[NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH];
static uint8_t m_head;
static uint8_t m_tail;
static uint8_t m_count;


static bridge_session_entry_t m_sessions[BRIDGE_SESSION_NODES];

static bridge_dup_entry_t m_dup[NRFCLAW_NINALINK_BRIDGE_DUP_CACHE];
static uint8_t m_dup_head;

static uint16_t m_received;
static uint16_t m_valid;
static uint16_t m_invalid;
static uint16_t m_dropped;
static uint16_t m_radio_dropped_base;
static uint16_t m_ack_sent;
static uint16_t m_duplicates;
static uint16_t m_foreign_network;
static uint8_t m_last_error;

static uint8_t m_tx_wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
static uint8_t m_tx_len;

static nrfclaw_ninalink_app_dl_status_t m_app;
static uint16_t m_next_app_sequence = 1U;

static nrfclaw_ninalink_command_dl_status_t m_cmd;
static uint16_t m_next_command_sequence = 1U;
static uint8_t m_cmd_args[NRFCLAW_NINALINK_COMMAND_DATA_MAX];
static uint8_t m_cmd_arg_len;

static nrfclaw_ninalink_command_discovery_status_t m_disc;
static uint16_t m_next_discovery_sequence = 1U;

static nrfclaw_ninalink_capability_discovery_status_t m_capdisc;
static uint16_t m_next_capability_discovery_sequence = 1U;


static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t crc16_ccitt_false(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0xFFFFU;
    uint8_t i;

    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (i = 0U; i < 8U; i++) {
            if (crc & 0x8000U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc <<= 1;
        }
    }
    return crc;
}

static void reset_queue(void)
{
    m_head = 0U;
    m_tail = 0U;
    m_count = 0U;
}

static void reset_dup_cache(void)
{
    memset(m_dup, 0, sizeof(m_dup));
    m_dup_head = 0U;
}

static int find_session(uint32_t node_id)
{
    uint8_t i;
    for (i = 0U; i < BRIDGE_SESSION_NODES; i++) {
        if (m_sessions[i].valid && m_sessions[i].node_id == node_id)
            return (int)i;
    }
    return -1;
}

static bool session_needs_commit(uint32_t node_id, uint32_t session_id)
{
    int found;
    if (session_id == 0U || session_id == 0xFFFFFFFFUL)
        return false;
    found = find_session(node_id);
    return found < 0 || m_sessions[(uint8_t)found].session_id != session_id;
}

static void forget_duplicates_for_node(uint32_t node_id)
{
    uint8_t i;
    for (i = 0U; i < NRFCLAW_NINALINK_BRIDGE_DUP_CACHE; i++) {
        if (m_dup[i].valid && m_dup[i].node_id == node_id)
            m_dup[i].valid = false;
    }
}

static void commit_session(uint32_t node_id, uint32_t session_id)
{
    int found;
    uint8_t i;

    if (session_id == 0U || session_id == 0xFFFFFFFFUL)
        return;

    found = find_session(node_id);
    if (found >= 0) {
        m_sessions[(uint8_t)found].session_id = session_id;
        return;
    }

    for (i = 0U; i < BRIDGE_SESSION_NODES; i++) {
        if (!m_sessions[i].valid) {
            m_sessions[i].valid = true;
            m_sessions[i].node_id = node_id;
            m_sessions[i].session_id = session_id;
            return;
        }
    }

    /* B5.3 cache itself is also bounded to eight nodes; deterministic fallback. */
    m_sessions[0].valid = true;
    m_sessions[0].node_id = node_id;
    m_sessions[0].session_id = session_id;
}

static bool extract_session_metadata(
    const nrfclaw_ninalink_frame_t *frame,
    uint32_t *session_id)
{
    nrfclaw_ninalink_value_entry_t entries[NRFCLAW_NINALINK_MAX_VALUE_ENTRIES];
    uint8_t count = 0U;
    uint8_t i;

    if (session_id)
        *session_id = 0U;
    if (!frame || !session_id)
        return false;
    if (frame->message_type != NRFCLAW_NINALINK_MSG_CAP_REPORT &&
        frame->message_type != NRFCLAW_NINALINK_MSG_CAP_EVENT)
        return false;
    if (nrfclaw_ninalink_parse_values(
            frame, entries, NRFCLAW_NINALINK_MAX_VALUE_ENTRIES, &count) !=
            NRFCLAW_NINALINK_MSG_OK)
        return false;

    for (i = 0U; i < count; i++) {
        if (entries[i].capability_id != NRFCLAW_NINALINK_META_SESSION_ID)
            continue;
        if (entries[i].channel != 0U ||
            entries[i].value.type != NRFCLAW_CAP_VALUE_U32 ||
            entries[i].value.v.u32 == 0U ||
            entries[i].value.v.u32 == 0xFFFFFFFFUL)
            return false;
        *session_id = entries[i].value.v.u32;
        return true;
    }
    return false;
}

static bool is_duplicate(const uint8_t *wire)
{
    uint16_t network_id = get_u16_le(&wire[4]);
    uint32_t node_id = get_u32_le(&wire[7]);
    uint16_t sequence = get_u16_le(&wire[11]);
    uint8_t message_type = wire[3];
    uint8_t i;

    for (i = 0U; i < NRFCLAW_NINALINK_BRIDGE_DUP_CACHE; i++) {
        if (m_dup[i].valid &&
            m_dup[i].network_id == network_id &&
            m_dup[i].node_id == node_id &&
            m_dup[i].sequence == sequence &&
            m_dup[i].message_type == message_type)
            return true;
    }
    return false;
}

static void remember_frame(const uint8_t *wire)
{
    bridge_dup_entry_t *e = &m_dup[m_dup_head];

    e->valid = true;
    e->network_id = get_u16_le(&wire[4]);
    e->node_id = get_u32_le(&wire[7]);
    e->sequence = get_u16_le(&wire[11]);
    e->message_type = wire[3];

    m_dup_head = (uint8_t)(
        (m_dup_head + 1U) % NRFCLAW_NINALINK_BRIDGE_DUP_CACHE);
}

static void tx_timer_handler(void *context)
{
    (void)context;
    m_tx_due = true;
}

static bool ensure_timer(void)
{
    if (m_timer_initialized)
        return true;

    if (app_timer_create(
            &m_bridge_tx_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            tx_timer_handler) != NRF_SUCCESS)
        return false;

    m_timer_initialized = true;
    return true;
}

static bool validate_semantic(const nrfclaw_ninalink_frame_t *frame)
{
    if (frame->message_type == NRFCLAW_NINALINK_MSG_CAP_REPORT ||
        frame->message_type == NRFCLAW_NINALINK_MSG_CAP_EVENT) {
        nrfclaw_ninalink_value_entry_t entries[12];
        uint8_t entry_count = 0U;

        return nrfclaw_ninalink_parse_values(
                   frame,
                   entries,
                   (uint8_t)(sizeof(entries) / sizeof(entries[0])),
                   &entry_count) == NRFCLAW_NINALINK_MSG_OK;
    }
    return true;
}

static bool queue_validated(const uint8_t *data,
                            uint8_t len,
                            int16_t rssi_x2,
                            int16_t snr_x4)
{
    bridge_queue_packet_t *p;

    if (m_count >= NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH) {
        m_dropped++;
        return false;
    }

    p = &m_queue[m_head];
    p->len = len;
    memcpy(p->data, data, len);
    p->rssi_x2 = rssi_x2;
    p->snr_x4 = snr_x4;

    m_head = (uint8_t)((m_head + 1U) % NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH);
    m_count++;
    return true;
}


static void consume_validated_queue(void)
{
    while (m_count != 0U) {
        bridge_queue_packet_t *packet = &m_queue[m_tail];
        nrfclaw_ninalink_frame_t frame;

        if (nrfclaw_ninalink_decode(packet->data, packet->len, &frame) ==
                NRFCLAW_NINALINK_OK &&
            (frame.message_type == NRFCLAW_NINALINK_MSG_CAP_REPORT ||
             frame.message_type == NRFCLAW_NINALINK_MSG_CAP_EVENT)) {
            nrfclaw_ninalink_value_entry_t entries[NRFCLAW_NINALINK_MAX_VALUE_ENTRIES];
            uint8_t entry_count = 0U;
            if (nrfclaw_ninalink_parse_values(
                    &frame, entries, NRFCLAW_NINALINK_MAX_VALUE_ENTRIES,
                    &entry_count) == NRFCLAW_NINALINK_MSG_OK) {
                uint8_t src_i;
                uint8_t dst_i = 0U;
                uint32_t session_id = 0U;

                for (src_i = 0U; src_i < entry_count; src_i++) {
                    if (entries[src_i].capability_id ==
                            NRFCLAW_NINALINK_META_SESSION_ID &&
                        entries[src_i].channel == 0U &&
                        entries[src_i].value.type == NRFCLAW_CAP_VALUE_U32) {
                        session_id = entries[src_i].value.v.u32;
                        continue;
                    }
                    if (dst_i != src_i)
                        entries[dst_i] = entries[src_i];
                    dst_i++;
                }

                if (dst_i != 0U) {
                    (void)nrfclaw_ninalink_state_cache_ingest_values_session(
                        frame.node_id, session_id, frame.sequence,
                        frame.message_type, entries, dst_i,
                        nrfclaw_rtc_now());
                }
            }
        }

        m_tail = (uint8_t)((m_tail + 1U) % NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH);
        m_count--;
    }
}


static bool restart_stream(void)
{
    if (nrfclaw_lora_diag_stream_active()) {
        m_restart_pending = false;
        return true;
    }

    if (!nrfclaw_lora_idle()) {
        m_restart_pending = true;
        return true;
    }

    if (nrfclaw_lora_diag_stream_start()) {
        m_restart_pending = false;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        return true;
    }

    m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_RESTART;
    m_restart_pending = true;
    return true;
}

static void build_ack(uint16_t network_id, uint16_t ack_seq)
{
    uint16_t crc;

    memset(m_tx_wire, 0, sizeof(m_tx_wire));
    m_tx_wire[0] = 0x4EU;
    m_tx_wire[1] = 0x01U;
    m_tx_wire[2] = 0x00U;
    m_tx_wire[3] = NRFCLAW_NINALINK_MSG_ACK;
    put_u16_le(&m_tx_wire[4], network_id);
    m_tx_wire[6] = 3U;
    put_u32_le(&m_tx_wire[7], NRF_FICR->DEVICEID[0]);
    put_u16_le(&m_tx_wire[11], ack_seq);
    put_u16_le(&m_tx_wire[13], ack_seq);
    m_tx_wire[15] = 0U;
    crc = crc16_ccitt_false(m_tx_wire, 16U);
    put_u16_le(&m_tx_wire[16], crc);
    m_tx_len = BRIDGE_ACK_FRAME_LEN;
}

static void build_cap_set(uint16_t network_id,
                          uint16_t reply_to_seq)
{
    uint16_t crc;

    memset(m_tx_wire, 0, sizeof(m_tx_wire));
    m_tx_wire[0] = 0x4EU;
    m_tx_wire[1] = 0x01U;
    m_tx_wire[2] = 0x01U; /* ACK_REQ */
    m_tx_wire[3] = NRFCLAW_NINALINK_MSG_CAP_SET;
    put_u16_le(&m_tx_wire[4], network_id);
    m_tx_wire[6] = 7U;
    put_u32_le(&m_tx_wire[7], m_app.target_node);
    put_u16_le(&m_tx_wire[11], m_app.command_seq);

    put_u16_le(&m_tx_wire[13], reply_to_seq);
    put_u16_le(&m_tx_wire[15], m_app.capability_id);
    m_tx_wire[17] = m_app.channel;
    m_tx_wire[18] = m_app.value_type;
    m_tx_wire[19] = m_app.requested_value;

    crc = crc16_ccitt_false(m_tx_wire, 20U);
    put_u16_le(&m_tx_wire[20], crc);
    m_tx_len = BRIDGE_CAP_SET_LEN;
}


static void build_command(uint16_t network_id,
                          uint16_t reply_to_seq)
{
    uint8_t payload_len;
    uint8_t crc_off;
    uint16_t crc;

    payload_len = (uint8_t)(COMMAND_PAYLOAD_BASE + m_cmd_arg_len);
    crc_off = (uint8_t)(13U + payload_len);

    memset(m_tx_wire, 0, sizeof(m_tx_wire));
    m_tx_wire[0] = 0x4EU;
    m_tx_wire[1] = 0x01U;
    m_tx_wire[2] = 0x01U; /* ACK_REQ */
    m_tx_wire[3] = NRFCLAW_NINALINK_MSG_COMMAND;
    put_u16_le(&m_tx_wire[4], network_id);
    m_tx_wire[6] = payload_len;
    put_u32_le(&m_tx_wire[7], m_cmd.target_node);
    put_u16_le(&m_tx_wire[11], m_cmd.command_seq);

    put_u16_le(&m_tx_wire[13], reply_to_seq);
    put_u16_le(&m_tx_wire[15], m_cmd.command_id);
    m_tx_wire[17] = m_cmd_arg_len;
    if (m_cmd_arg_len)
        memcpy(&m_tx_wire[18], m_cmd_args, m_cmd_arg_len);

    crc = crc16_ccitt_false(m_tx_wire, crc_off);
    put_u16_le(&m_tx_wire[crc_off], crc);
    m_tx_len = (uint8_t)(crc_off + 2U);
}

static void build_command_discovery_request(uint16_t network_id,
                                            uint16_t reply_to_seq)
{
    uint16_t crc;

    memset(m_tx_wire, 0, sizeof(m_tx_wire));
    m_tx_wire[0] = 0x4EU;
    m_tx_wire[1] = 0x01U;
    m_tx_wire[2] = 0x01U;
    m_tx_wire[3] = NRFCLAW_NINALINK_MSG_COMMANDS_REQUEST;
    put_u16_le(&m_tx_wire[4], network_id);
    m_tx_wire[6] = 3U;
    put_u32_le(&m_tx_wire[7], m_disc.target_node);
    put_u16_le(&m_tx_wire[11], m_disc.request_seq);
    put_u16_le(&m_tx_wire[13], reply_to_seq);
    m_tx_wire[15] = m_disc.start_index;

    crc = crc16_ccitt_false(m_tx_wire, 16U);
    put_u16_le(&m_tx_wire[16], crc);
    m_tx_len = 18U;
}

static bool schedule_tx(bridge_tx_kind_t kind)
{
    if (!ensure_timer()) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER;
        return false;
    }

    m_radio_dropped_base = (uint16_t)(
        m_radio_dropped_base + nrfclaw_lora_diag_stream_dropped());

    (void)nrfclaw_lora_cancel_receive();

    m_tx_due = false;
    if (app_timer_start(
            m_bridge_tx_timer,
            APP_TIMER_TICKS(BRIDGE_TX_DELAY_MS),
            NULL) != NRF_SUCCESS) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER;
        (void)restart_stream();
        return false;
    }

    m_tx_kind = kind;
    m_tx_state = BRIDGE_TX_DELAY;
    return true;
}

static bool app_matches_node(uint32_t node_id)
{
    return m_app.pending && m_app.target_node == node_id;
}

static bool command_matches_node(uint32_t node_id)
{
    return m_cmd.pending && m_cmd.target_node == node_id;
}

static bool discovery_matches_node(uint32_t node_id)
{
    return m_disc.pending && m_disc.target_node == node_id;
}

static void build_capability_discovery_request(
    uint16_t network_id,
    uint16_t reply_to_seq)
{
    uint16_t crc;

    memset(m_tx_wire, 0, sizeof(m_tx_wire));
    m_tx_wire[0] = 0x4EU;
    m_tx_wire[1] = 0x01U;
    m_tx_wire[2] = 0x01U;
    m_tx_wire[3] = NRFCLAW_NINALINK_MSG_CAPS_REQUEST;
    put_u16_le(&m_tx_wire[4], network_id);
    m_tx_wire[6] = 3U;
    put_u32_le(&m_tx_wire[7], m_capdisc.target_node);
    put_u16_le(&m_tx_wire[11], m_capdisc.request_seq);
    put_u16_le(&m_tx_wire[13], reply_to_seq);
    m_tx_wire[15] = m_capdisc.page_index;

    crc = crc16_ccitt_false(m_tx_wire, 16U);
    put_u16_le(&m_tx_wire[16], crc);
    m_tx_len = 18U;
}

static bool capability_discovery_matches_node(uint32_t node_id)
{
    return m_capdisc.pending && m_capdisc.target_node == node_id;
}

static bool schedule_response_for_uplink(const uint8_t *wire)
{
    uint16_t network_id = get_u16_le(&wire[4]);
    uint16_t uplink_seq = get_u16_le(&wire[11]);
    uint32_t node_id = get_u32_le(&wire[7]);

    /*
     * COMMAND has priority over CAP_SET if both somehow become pending.
     * Queue APIs normally prevent that, but the priority keeps behavior
     * deterministic if state is restored/extended later.
     */
    if (command_matches_node(node_id)) {
        build_command(network_id, uplink_seq);
        return schedule_tx(BRIDGE_TX_KIND_COMMAND);
    }

    if (discovery_matches_node(node_id)) {
        build_command_discovery_request(network_id, uplink_seq);
        return schedule_tx(BRIDGE_TX_KIND_COMMAND_DISCOVERY);
    }

        if (capability_discovery_matches_node(node_id)) {
        build_capability_discovery_request(network_id, uplink_seq);
        return schedule_tx(BRIDGE_TX_KIND_CAPABILITY_DISCOVERY);
    }

if (app_matches_node(node_id)) {
        build_cap_set(network_id, uplink_seq);
        return schedule_tx(BRIDGE_TX_KIND_CAP_SET);
    }

    build_ack(network_id, uplink_seq);
    return schedule_tx(BRIDGE_TX_KIND_ACK);
}

static bool accept_app_result(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t ack_seq;

    if (len != BRIDGE_ACK_FRAME_LEN)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_ACK || wire[6] != 3U)
        return false;

    ack_seq = get_u16_le(&wire[13]);
    if (ack_seq != m_app.command_seq)
        return false;

    m_app.result = wire[15];
    m_app.cache_committed = false;
    m_app.cache_error = 0U;

    if (m_app.result == NRFCLAW_NINALINK_APP_OK) {
        nrfclaw_ninalink_value_entry_t entry;
        bool cache_value_valid = true;

        memset(&entry, 0, sizeof(entry));
        entry.capability_id = m_app.capability_id;
        entry.channel = m_app.channel;
        entry.value.type = m_app.value_type;

        switch (m_app.value_type) {
            case NRFCLAW_CAP_VALUE_BOOL:
                entry.value.v.boolean = m_app.requested_value != 0U;
                break;

            case NRFCLAW_CAP_VALUE_U8:
            case NRFCLAW_CAP_VALUE_ENUM8:
                entry.value.v.u8 = m_app.requested_value;
                break;

            case NRFCLAW_CAP_VALUE_S8:
                entry.value.v.s8 = (int8_t)m_app.requested_value;
                break;

            default:
                cache_value_valid = false;
                break;
        }

        if (cache_value_valid &&
            nrfclaw_ninalink_state_cache_upsert_query_values(
                m_app.target_node, &entry, 1U, nrfclaw_rtc_now())) {
            m_app.cache_committed = true;
        } else {
            m_app.cache_error = 1U;
        }
    }

    m_app.pending = false;
    m_app.state = NRFCLAW_NINALINK_APP_DL_DONE;
    m_app.completed_count++;

    ((void)0);

    return true;
}

static bool accept_command_result(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint8_t payload_len;
    uint16_t command_seq;
    uint8_t result_len;

    if (len < 19U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_COMMAND_RESULT)
        return false;

    payload_len = wire[6];
    if (payload_len < COMMAND_RESULT_BASE)
        return false;

    result_len = wire[16];
    if ((uint8_t)(COMMAND_RESULT_BASE + result_len) != payload_len)
        return false;

    if (result_len > NRFCLAW_NINALINK_COMMAND_DATA_MAX)
        return false;

    if ((uint16_t)13U + payload_len + 2U != len)
        return false;

    command_seq = get_u16_le(&wire[13]);
    if (command_seq != m_cmd.command_seq)
        return false;

    m_cmd.result = wire[15];
    m_cmd.result_len = result_len;
    memset(m_cmd.result_data, 0, sizeof(m_cmd.result_data));
    if (result_len)
        memcpy(m_cmd.result_data, &wire[17], result_len);

    nrfclaw_ninalink_query_on_command_result(
        m_cmd.target_node,
        m_cmd.command_seq,
        m_cmd.command_id,
        m_cmd.result,
        m_cmd.result_data,
        m_cmd.result_len,
        nrfclaw_rtc_now());

    m_cmd.pending = false;
    m_cmd.state = NRFCLAW_NINALINK_APP_DL_DONE;
    m_cmd.completed_count++;

    ((void)0);

    return true;
}

static bool accept_command_discovery_response(
    const uint8_t *wire,
    uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    nrfclaw_ninalink_command_discovery_page_t page;
    uint32_t node_id;
    uint16_t request_seq;

    if (len < 19U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_COMMANDS_RESPONSE)
        return false;

    node_id = get_u32_le(&wire[7]);
    request_seq = get_u16_le(&wire[11]);

    if (node_id != m_disc.target_node ||
        request_seq != m_disc.request_seq)
        return false;

    if (!nrfclaw_ninalink_command_discovery_parse_page(
            &wire[13], wire[6], &page))
        return false;

    if (page.start_index != m_disc.start_index)
        return false;

    m_disc.registry_version = page.registry_version;
    m_disc.total_count = page.total_count;
    m_disc.count = page.count;
    memset(m_disc.descriptors, 0, sizeof(m_disc.descriptors));
    if (page.count)
        memcpy(
            m_disc.descriptors,
            page.descriptors,
            (size_t)page.count * sizeof(page.descriptors[0]));

    m_disc.pending = false;
    m_disc.state = NRFCLAW_NINALINK_APP_DL_DONE;
    m_disc.completed_count++;

    ((void)0);

    return true;
}

static bool accept_capability_discovery_response(
    const uint8_t *wire,
    uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint32_t node_id;
    uint16_t request_seq;
    uint8_t registry_version = 0U;
    uint8_t page_index = 0U;
    uint8_t count = 0U;
    bool more = false;
    nrfclaw_ninalink_capability_descriptor_t
        descriptors[NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX];

    if (len < 19U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (frame.message_type != NRFCLAW_NINALINK_MSG_CAPS_RESPONSE)
        return false;

    node_id = get_u32_le(&wire[7]);
    request_seq = get_u16_le(&wire[11]);
    if (node_id != m_capdisc.target_node ||
        request_seq != m_capdisc.request_seq)
        return false;

    memset(descriptors, 0, sizeof(descriptors));
    if (nrfclaw_ninalink_parse_caps_response(
            &frame, &registry_version, &page_index, descriptors,
            NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX,
            &count, &more) != NRFCLAW_NINALINK_MSG_OK)
        return false;

    if (registry_version != NRFCLAW_CAPABILITY_REGISTRY_VERSION ||
        page_index != m_capdisc.page_index)
        return false;

    m_capdisc.registry_version = registry_version;
    m_capdisc.count = count;
    m_capdisc.more = more;
    memset(m_capdisc.descriptors, 0, sizeof(m_capdisc.descriptors));
    if (count)
        memcpy(m_capdisc.descriptors, descriptors,
               (size_t)count * sizeof(descriptors[0]));

    (void)nrfclaw_ninalink_cap_inventory_store_page(
        node_id, registry_version, page_index, descriptors, count, more);

    m_capdisc.pending = false;
    m_capdisc.state = NRFCLAW_NINALINK_APP_DL_DONE;
    m_capdisc.completed_count++;
    return true;
}


bool nrfclaw_ninalink_bridge_start(void)
{
    if (m_active)
        return true;

    if (!ensure_timer()) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER;
        return false;
    }

    reset_queue();
    reset_dup_cache();
    memset(m_sessions, 0, sizeof(m_sessions));

    m_received = 0U;
    m_valid = 0U;
    m_invalid = 0U;
    m_dropped = 0U;
    m_radio_dropped_base = 0U;
    m_ack_sent = 0U;
    m_duplicates = 0U;
    m_foreign_network = 0U;
    m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
    m_tx_state = BRIDGE_TX_IDLE;
    m_tx_kind = BRIDGE_TX_KIND_NONE;
    m_tx_due = false;
    m_restart_pending = false;

    memset(&m_app, 0, sizeof(m_app));
    m_app.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    m_app.result = 0xFFU;

    memset(&m_cmd, 0, sizeof(m_cmd));
    m_cmd.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    m_cmd.result = 0xFFU;
    m_cmd_arg_len = 0U;
    nrfclaw_ninalink_query_reset();

    memset(&m_disc, 0, sizeof(m_disc));
    m_disc.state = NRFCLAW_NINALINK_APP_DL_IDLE;

    memset(&m_capdisc, 0, sizeof(m_capdisc));
    m_capdisc.state = NRFCLAW_NINALINK_APP_DL_IDLE;

    nrfclaw_ninalink_auto_discovery_reset_runtime();


    if (!nrfclaw_lora_diag_stream_start())
        return false;

    m_active = true;
    ((void)0);
    return true;
}

void nrfclaw_ninalink_bridge_stop(void)
{
    if (m_timer_initialized)
        (void)app_timer_stop(m_bridge_tx_timer);

    if (m_active) {
        if (nrfclaw_lora_diag_stream_active()) {
            m_radio_dropped_base = (uint16_t)(
                m_radio_dropped_base + nrfclaw_lora_diag_stream_dropped());
        }
        (void)nrfclaw_lora_cancel_receive();
    }

    m_active = false;
    m_tx_state = BRIDGE_TX_IDLE;
    m_tx_kind = BRIDGE_TX_KIND_NONE;
    m_tx_due = false;
    m_restart_pending = false;
    reset_queue();
}


bool nrfclaw_ninalink_bridge_queue_cap_set(
    uint32_t target_node,
    uint16_t capability_id,
    uint8_t channel,
    uint8_t value_type,
    uint8_t value)
{
    if (!m_active || m_app.pending || m_cmd.pending ||
        m_tx_state == BRIDGE_TX_WAIT_APP_RESULT)
        return false;

    if (target_node == 0U || target_node == 0xFFFFFFFFUL ||
        capability_id == 0U)
        return false;

    if (value_type != NRFCLAW_CAP_VALUE_BOOL &&
        value_type != NRFCLAW_CAP_VALUE_U8 &&
        value_type != NRFCLAW_CAP_VALUE_S8 &&
        value_type != NRFCLAW_CAP_VALUE_ENUM8)
        return false;

    m_app.pending = true;
    m_app.target_node = target_node;
    m_app.command_seq = m_next_app_sequence++;
    m_app.capability_id = capability_id;
    m_app.channel = channel;
    m_app.value_type = value_type;
    m_app.requested_value = value;
    m_app.cache_committed = false;
    m_app.cache_error = 0U;
    m_app.state = NRFCLAW_NINALINK_APP_DL_PENDING;
    m_app.result = 0xFFU;

    ((void)0);

    return true;
}

bool nrfclaw_ninalink_bridge_queue_tracking(uint32_t target_node, bool active)
{
    return nrfclaw_ninalink_bridge_queue_cap_set(
        target_node,
        NRFCLAW_SEMCAP_TRACKING_ACTIVE,
        0U,
        NRFCLAW_CAP_VALUE_BOOL,
        active ? 1U : 0U);
}


void nrfclaw_ninalink_bridge_get_app_status(
    nrfclaw_ninalink_app_dl_status_t *out)
{
    if (out)
        *out = m_app;
}

bool nrfclaw_ninalink_bridge_queue_command(
    uint32_t target_node,
    uint16_t command_id,
    const uint8_t *args,
    uint8_t arg_len)
{
    if (!m_active || m_app.pending || m_cmd.pending || m_disc.pending ||
        m_capdisc.pending ||
        m_tx_state == BRIDGE_TX_WAIT_APP_RESULT)
        return false;

    if (arg_len > NRFCLAW_NINALINK_COMMAND_DATA_MAX)
        return false;

    if (arg_len && !args)
        return false;

    memset(&m_cmd, 0, sizeof(m_cmd));
    m_cmd.pending = true;
    m_cmd.target_node = target_node;
    m_cmd.command_seq = m_next_command_sequence++;
    m_cmd.command_id = command_id;
    m_cmd.state = NRFCLAW_NINALINK_APP_DL_PENDING;
    m_cmd.result = 0xFFU;

    m_cmd_arg_len = arg_len;
    memset(m_cmd_args, 0, sizeof(m_cmd_args));
    if (arg_len)
        memcpy(m_cmd_args, args, arg_len);

    ((void)0);

    return true;
}

void nrfclaw_ninalink_bridge_get_command_status(
    nrfclaw_ninalink_command_dl_status_t *out)
{
    if (out)
        *out = m_cmd;
}

bool nrfclaw_ninalink_bridge_queue_command_discovery(
    uint32_t target_node,
    uint8_t start_index)
{
    if (!m_active || m_app.pending || m_cmd.pending || m_disc.pending ||
        m_capdisc.pending ||
        m_tx_state == BRIDGE_TX_WAIT_APP_RESULT)
        return false;

    memset(&m_disc, 0, sizeof(m_disc));
    m_disc.pending = true;
    m_disc.target_node = target_node;
    m_disc.request_seq = m_next_discovery_sequence++;
    m_disc.start_index = start_index;
    m_disc.state = NRFCLAW_NINALINK_APP_DL_PENDING;

    ((void)0);

    return true;
}

void nrfclaw_ninalink_bridge_get_command_discovery_status(
    nrfclaw_ninalink_command_discovery_status_t *out)
{
    if (out)
        *out = m_disc;
}

bool nrfclaw_ninalink_bridge_queue_capability_discovery(
    uint32_t target_node,
    uint8_t page_index)
{
    if (!m_active || m_app.pending || m_cmd.pending || m_disc.pending ||
        m_capdisc.pending ||
        m_tx_state == BRIDGE_TX_WAIT_APP_RESULT)
        return false;

    memset(&m_capdisc, 0, sizeof(m_capdisc));
    m_capdisc.pending = true;
    m_capdisc.target_node = target_node;
    m_capdisc.request_seq = m_next_capability_discovery_sequence++;
    m_capdisc.page_index = page_index;
    m_capdisc.state = NRFCLAW_NINALINK_APP_DL_PENDING;

    ((void)0);

    return true;
}

void nrfclaw_ninalink_bridge_get_capability_discovery_status(
    nrfclaw_ninalink_capability_discovery_status_t *out)
{
    if (out)
        *out = m_capdisc;
}

bool nrfclaw_ninalink_bridge_cancel_capability_discovery(
    uint32_t target_node,
    uint16_t request_seq)
{
    if (!m_capdisc.pending ||
        m_capdisc.target_node != target_node ||
        m_capdisc.request_seq != request_seq)
        return false;
    if (m_tx_state == BRIDGE_TX_WAIT_APP_RESULT ||
        m_tx_kind == BRIDGE_TX_KIND_CAPABILITY_DISCOVERY)
        return false;
    m_capdisc.pending = false;
    m_capdisc.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    return true;
}

bool nrfclaw_ninalink_bridge_cancel_command_discovery(
    uint32_t target_node,
    uint16_t request_seq)
{
    if (!m_disc.pending ||
        m_disc.target_node != target_node ||
        m_disc.request_seq != request_seq)
        return false;
    if (m_tx_state == BRIDGE_TX_WAIT_APP_RESULT ||
        m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY)
        return false;
    m_disc.pending = false;
    m_disc.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    return true;
}

void nrfclaw_ninalink_bridge_process(void)
{
    uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t len;
    int16_t rssi_x2;
    int16_t snr_x4;

    if (!m_active)
        return;

    nrfclaw_ninalink_auto_discovery_process();

    /*
     * Complete a deferred continuous-RX rearm before processing application
     * traffic. This closes the RX_DONE -> stream-start race that can strand
     * the bridge with a pending command and Sent=0.
     */
    if (m_restart_pending) {
        if (nrfclaw_lora_diag_stream_active()) {
            m_restart_pending = false;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        } else {
            if (!nrfclaw_lora_idle())
                return;

            if (!nrfclaw_lora_diag_stream_start()) {
                m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_RESTART;
                return;
            }

            m_restart_pending = false;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        }
    }

    if (m_tx_state == BRIDGE_TX_DELAY) {
        if (!m_tx_due)
            return;

        m_tx_due = false;
        if (!nrfclaw_lora_send_async(m_tx_wire, m_tx_len)) {
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TX;
            m_tx_state = BRIDGE_TX_IDLE;
            (void)restart_stream();
            return;
        }

        m_tx_state = BRIDGE_TX_WAIT;
        return;
    }

    if (m_tx_state == BRIDGE_TX_WAIT) {
        if (!nrfclaw_lora_idle())
            return;

        if (m_tx_kind == BRIDGE_TX_KIND_ACK) {
            m_ack_sent++;
            m_tx_kind = BRIDGE_TX_KIND_NONE;
            m_tx_state = BRIDGE_TX_IDLE;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
            (void)restart_stream();
            return;
        }

        if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET ||
            m_tx_kind == BRIDGE_TX_KIND_COMMAND ||
            m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY ||
            m_tx_kind == BRIDGE_TX_KIND_CAPABILITY_DISCOVERY) {
            if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET) {
                m_app.sent_count++;
                m_app.state = NRFCLAW_NINALINK_APP_DL_WAIT_RESULT;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND) {
                m_cmd.sent_count++;
                m_cmd.state = NRFCLAW_NINALINK_APP_DL_WAIT_RESULT;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY) {
                m_disc.sent_count++;
                m_disc.state = NRFCLAW_NINALINK_APP_DL_WAIT_RESULT;
            } else {
                m_capdisc.sent_count++;
                m_capdisc.state = NRFCLAW_NINALINK_APP_DL_WAIT_RESULT;
            }


            if (!nrfclaw_lora_receive_window_async(
                    BRIDGE_APP_RESULT_RX_MS)) {
                if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET)
                    m_app.state = NRFCLAW_NINALINK_APP_DL_PENDING;
                else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND)
                    m_cmd.state = NRFCLAW_NINALINK_APP_DL_PENDING;
                else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY)
                    m_disc.state = NRFCLAW_NINALINK_APP_DL_PENDING;
                else
                    m_capdisc.state = NRFCLAW_NINALINK_APP_DL_PENDING;

                m_tx_kind = BRIDGE_TX_KIND_NONE;
                m_tx_state = BRIDGE_TX_IDLE;
                (void)restart_stream();
                return;
            }

            m_tx_state = BRIDGE_TX_WAIT_APP_RESULT;
            return;
        }
    }

    if (m_tx_state == BRIDGE_TX_WAIT_APP_RESULT) {
        if (nrfclaw_lora_rx_ready()) {
            uint8_t result_wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
            uint8_t result_len = 0U;
            bool accepted = false;
            bool took_rx;


            took_rx = nrfclaw_lora_take_rx(
                result_wire,
                &result_len,
                sizeof(result_wire));


            if (took_rx) {
                if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET)
                    accepted = accept_app_result(result_wire, result_len);
                else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND)
                    accepted = accept_command_result(result_wire, result_len);
                else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY)
                    accepted = accept_command_discovery_response(
                        result_wire, result_len);
                else if (m_tx_kind == BRIDGE_TX_KIND_CAPABILITY_DISCOVERY)
                    accepted = accept_capability_discovery_response(
                        result_wire, result_len);
            }

            if (accepted) {
                m_tx_kind = BRIDGE_TX_KIND_NONE;
                m_tx_state = BRIDGE_TX_IDLE;
                m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
                (void)restart_stream();
                return;
            }

            if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET) {
                if (m_app.timeout_count != 0xFFU)
                    m_app.timeout_count++;
                m_app.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND) {
                if (m_cmd.timeout_count != 0xFFU)
                    m_cmd.timeout_count++;
                m_cmd.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY) {
                if (m_disc.timeout_count != 0xFFU)
                    m_disc.timeout_count++;
                m_disc.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_CAPABILITY_DISCOVERY) {
                if (m_capdisc.timeout_count != 0xFFU)
                    m_capdisc.timeout_count++;
                m_capdisc.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            }

            m_tx_kind = BRIDGE_TX_KIND_NONE;
            m_tx_state = BRIDGE_TX_IDLE;
            (void)restart_stream();
            return;
        }

        if (!nrfclaw_lora_rx_active()) {
            if (m_tx_kind == BRIDGE_TX_KIND_CAP_SET) {
                if (m_app.timeout_count != 0xFFU)
                    m_app.timeout_count++;
                m_app.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND) {
                if (m_cmd.timeout_count != 0xFFU)
                    m_cmd.timeout_count++;
                m_cmd.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_COMMAND_DISCOVERY) {
                if (m_disc.timeout_count != 0xFFU)
                    m_disc.timeout_count++;
                m_disc.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            } else if (m_tx_kind == BRIDGE_TX_KIND_CAPABILITY_DISCOVERY) {
                if (m_capdisc.timeout_count != 0xFFU)
                    m_capdisc.timeout_count++;
                m_capdisc.state = NRFCLAW_NINALINK_APP_DL_PENDING;
            }

            m_tx_kind = BRIDGE_TX_KIND_NONE;
            m_tx_state = BRIDGE_TX_IDLE;
            (void)restart_stream();
        }
        return;
    }

    if (!nrfclaw_lora_diag_stream_active()) {
        m_active = false;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_RADIO_STOPPED;
        return;
    }

    while (nrfclaw_lora_diag_stream_take(
               wire,
               &len,
               sizeof(wire),
               &rssi_x2,
               &snr_x4)) {
        nrfclaw_ninalink_frame_t frame;
        nrfclaw_ninalink_status_t st;
        bool ack_req;
        uint32_t frame_session_id = 0U;
        bool has_session;
        bool session_changed;

        m_received++;

        st = nrfclaw_ninalink_decode(wire, len, &frame);
        if (st != NRFCLAW_NINALINK_OK) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_CORE;
            continue;
        }

        /* B7.6f2l1: reject a foreign NinaLink network before registry,
         * duplicate tracking, semantic validation, ACK or cache ingestion.
         * network_id already exists in the frozen v1 header, so this adds
         * zero bytes and zero LoRa airtime to the protocol. */
        if (frame.network_id != nrfclaw_ninalink_network_id()) {
            if (m_foreign_network != 0xFFFFU)
                m_foreign_network++;
            continue;
        }

        if (!validate_semantic(&frame)) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_SEMANTIC;
            continue;
        }

        nrfclaw_ninalink_node_registry_observe(
            &frame,
            rssi_x2,
            snr_x4,
            nrfclaw_rtc_now());

        ack_req = (wire[2] & 0x01U) != 0U;
        has_session = extract_session_metadata(&frame, &frame_session_id);
        session_changed = has_session &&
            session_needs_commit(frame.node_id, frame_session_id);

        if (!session_changed && is_duplicate(wire)) {
            m_duplicates++;

            if (ack_req) {
                nrfclaw_ninalink_auto_discovery_on_contact(
                    frame.node_id);
                (void)schedule_response_for_uplink(wire);
                break;
            }
            continue;
        }

        if (!queue_validated(wire, len, rssi_x2, snr_x4))
            continue;

        if (session_changed) {
            forget_duplicates_for_node(frame.node_id);
            commit_session(frame.node_id, frame_session_id);
        }

        remember_frame(wire);
        m_valid++;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;

        consume_validated_queue();

        if (ack_req) {
            nrfclaw_ninalink_auto_discovery_on_contact(
                frame.node_id);
            (void)schedule_response_for_uplink(wire);
            break;
        }
    }
}


void nrfclaw_ninalink_bridge_get_status(nrfclaw_ninalink_bridge_status_t *out)
{
    uint16_t current_radio_dropped = 0U;

    if (!out)
        return;

    if (nrfclaw_lora_diag_stream_active())
        current_radio_dropped = nrfclaw_lora_diag_stream_dropped();

    out->active = m_active;
    out->queued = m_count;
    out->received = m_received;
    out->valid = m_valid;
    out->invalid = m_invalid;
    out->dropped = m_dropped;
    out->radio_dropped = (uint16_t)(
        m_radio_dropped_base + current_radio_dropped);
    out->last_error = m_last_error;
    out->ack_sent = m_ack_sent;
    out->duplicates = m_duplicates;
    out->foreign_network = m_foreign_network;
}
