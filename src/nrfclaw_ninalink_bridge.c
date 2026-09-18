#include "nrfclaw_ninalink_bridge.h"

#include "app_timer.h"
#include "nrf.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink_msg.h"

#include "SEGGER_RTT.h"

#include <string.h>

#define BRIDGE_ACK_DELAY_MS 60U
#define BRIDGE_ACK_FRAME_LEN 18U

typedef struct {
    uint8_t len;
    uint8_t data[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    int16_t rssi_x2;
    int16_t snr_x4;
} bridge_queue_packet_t;

typedef enum {
    BRIDGE_ACK_IDLE = 0,
    BRIDGE_ACK_DELAY,
    BRIDGE_ACK_WAIT_TX
} bridge_ack_state_t;

APP_TIMER_DEF(m_bridge_ack_timer);

static bool m_active;
static bool m_timer_initialized;
static volatile bool m_ack_due;
static bridge_ack_state_t m_ack_state;
static bridge_queue_packet_t m_queue[NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH];
static uint8_t m_head;
static uint8_t m_tail;
static uint8_t m_count;
static uint16_t m_received;
static uint16_t m_valid;
static uint16_t m_invalid;
static uint16_t m_dropped;
static uint16_t m_radio_dropped_base;
static uint16_t m_ack_sent;
static uint8_t m_last_error;
static uint8_t m_ack_wire[BRIDGE_ACK_FRAME_LEN];

static void reset_queue(void)
{
    m_head = 0U;
    m_tail = 0U;
    m_count = 0U;
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

static void ack_timer_handler(void *context)
{
    (void)context;
    m_ack_due = true;
}

static bool ensure_timer(void)
{
    if (m_timer_initialized)
        return true;
    if (app_timer_create(&m_bridge_ack_timer,
                         APP_TIMER_MODE_SINGLE_SHOT,
                         ack_timer_handler) != NRF_SUCCESS)
        return false;
    m_timer_initialized = true;
    return true;
}

static void build_ack(uint16_t network_id, uint16_t ack_seq)
{
    uint16_t crc;
    memset(m_ack_wire, 0, sizeof(m_ack_wire));
    m_ack_wire[0] = 0x4EU;
    m_ack_wire[1] = 0x01U;
    m_ack_wire[2] = 0x00U;
    m_ack_wire[3] = NRFCLAW_NINALINK_MSG_ACK;
    put_u16_le(&m_ack_wire[4], network_id);
    m_ack_wire[6] = 3U;
    put_u32_le(&m_ack_wire[7], NRF_FICR->DEVICEID[0]);
    put_u16_le(&m_ack_wire[11], ack_seq);
    put_u16_le(&m_ack_wire[13], ack_seq);
    m_ack_wire[15] = 0U;
    crc = crc16_ccitt_false(m_ack_wire, 16U);
    put_u16_le(&m_ack_wire[16], crc);
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

static void queue_validated(const uint8_t *data,
                            uint8_t len,
                            int16_t rssi_x2,
                            int16_t snr_x4)
{
    bridge_queue_packet_t *p;
    if (m_count >= NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH) {
        m_dropped++;
        return;
    }
    p = &m_queue[m_head];
    p->len = len;
    memcpy(p->data, data, len);
    p->rssi_x2 = rssi_x2;
    p->snr_x4 = snr_x4;
    m_head = (uint8_t)((m_head + 1U) % NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH);
    m_count++;
}

static bool restart_stream(void)
{
    if (!nrfclaw_lora_diag_stream_start()) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_RESTART;
        m_active = false;
        return false;
    }
    return true;
}

static bool schedule_ack(const uint8_t *wire)
{
    uint16_t network_id;
    uint16_t ack_seq;
    if (!ensure_timer()) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER;
        return false;
    }
    network_id = (uint16_t)wire[4] | ((uint16_t)wire[5] << 8);
    ack_seq = (uint16_t)wire[11] | ((uint16_t)wire[12] << 8);
    build_ack(network_id, ack_seq);
    m_radio_dropped_base = (uint16_t)(
        m_radio_dropped_base + nrfclaw_lora_diag_stream_dropped());
    (void)nrfclaw_lora_cancel_receive();
    m_ack_due = false;
    if (app_timer_start(m_bridge_ack_timer,
                        APP_TIMER_TICKS(BRIDGE_ACK_DELAY_MS),
                        NULL) != NRF_SUCCESS) {
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER;
        (void)restart_stream();
        return false;
    }
    m_ack_state = BRIDGE_ACK_DELAY;
    SEGGER_RTT_printf(
        0,
        "NINALINK B4.3 BRIDGE: ACK scheduled seq=%u delay=%u ms\r\n",
        (unsigned)ack_seq,
        (unsigned)BRIDGE_ACK_DELAY_MS);
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
    m_received = 0U;
    m_valid = 0U;
    m_invalid = 0U;
    m_dropped = 0U;
    m_radio_dropped_base = 0U;
    m_ack_sent = 0U;
    m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
    m_ack_state = BRIDGE_ACK_IDLE;
    m_ack_due = false;
    if (!nrfclaw_lora_diag_stream_start())
        return false;
    m_active = true;
    SEGGER_RTT_WriteString(0, "NINALINK B4.3 BRIDGE: RX active\r\n");
    return true;
}

void nrfclaw_ninalink_bridge_stop(void)
{
    if (m_timer_initialized)
        (void)app_timer_stop(m_bridge_ack_timer);
    if (m_active) {
        if (nrfclaw_lora_diag_stream_active()) {
            m_radio_dropped_base = (uint16_t)(
                m_radio_dropped_base + nrfclaw_lora_diag_stream_dropped());
        }
        (void)nrfclaw_lora_cancel_receive();
    }
    m_active = false;
    m_ack_state = BRIDGE_ACK_IDLE;
    m_ack_due = false;
    reset_queue();
    SEGGER_RTT_WriteString(0, "NINALINK B4.3 BRIDGE: RX stopped\r\n");
}

void nrfclaw_ninalink_bridge_process(void)
{
    uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t len;
    int16_t rssi_x2;
    int16_t snr_x4;

    if (!m_active)
        return;

    if (m_ack_state == BRIDGE_ACK_DELAY) {
        if (!m_ack_due)
            return;
        m_ack_due = false;
        if (!nrfclaw_lora_send_async(m_ack_wire, (uint8_t)sizeof(m_ack_wire))) {
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TX;
            m_ack_state = BRIDGE_ACK_IDLE;
            (void)restart_stream();
            return;
        }
        m_ack_state = BRIDGE_ACK_WAIT_TX;
        return;
    }

    if (m_ack_state == BRIDGE_ACK_WAIT_TX) {
        if (!nrfclaw_lora_idle())
            return;
        m_ack_sent++;
        m_ack_state = BRIDGE_ACK_IDLE;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        SEGGER_RTT_printf(
            0,
            "NINALINK B4.3 BRIDGE: ACK TX complete count=%u\r\n",
            (unsigned)m_ack_sent);
        (void)restart_stream();
        return;
    }

    if (!nrfclaw_lora_diag_stream_active()) {
        m_active = false;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_RADIO_STOPPED;
        return;
    }

    while (nrfclaw_lora_diag_stream_take(
               wire, &len, sizeof(wire), &rssi_x2, &snr_x4)) {
        nrfclaw_ninalink_frame_t frame;
        nrfclaw_ninalink_status_t st;
        bool ack_req;

        m_received++;
        st = nrfclaw_ninalink_decode(wire, len, &frame);
        if (st != NRFCLAW_NINALINK_OK) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_CORE;
            continue;
        }
        if (!validate_semantic(&frame)) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_SEMANTIC;
            continue;
        }

        m_valid++;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        queue_validated(wire, len, rssi_x2, snr_x4);
        ack_req = (wire[2] & 0x01U) != 0U;

        SEGGER_RTT_printf(
            0,
            "NINALINK B4.3 BRIDGE: valid type=0x%02X node=0x%08lX seq=%u len=%u ack=%u\r\n",
            (unsigned)frame.message_type,
            (unsigned long)frame.node_id,
            (unsigned)frame.sequence,
            (unsigned)len,
            ack_req ? 1U : 0U);

        if (ack_req) {
            (void)schedule_ack(wire);
            break;
        }
    }
}

bool nrfclaw_ninalink_bridge_take(nrfclaw_ninalink_bridge_packet_t *out)
{
    bridge_queue_packet_t const *p;
    if (!out || m_count == 0U)
        return false;
    p = &m_queue[m_tail];
    out->len = p->len;
    memcpy(out->data, p->data, p->len);
    out->rssi_x2 = p->rssi_x2;
    out->snr_x4 = p->snr_x4;
    m_tail = (uint8_t)((m_tail + 1U) % NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH);
    m_count--;
    return true;
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
    out->radio_dropped = (uint16_t)(m_radio_dropped_base + current_radio_dropped);
    out->last_error = m_last_error;
    out->ack_sent = m_ack_sent;
}
