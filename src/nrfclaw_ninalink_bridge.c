#include "nrfclaw_ninalink_bridge.h"

#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink_msg.h"

#include "SEGGER_RTT.h"

#include <string.h>

typedef struct {
    uint8_t len;
    uint8_t data[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    int16_t rssi_x2;
    int16_t snr_x4;
} bridge_queue_packet_t;

static bool m_active;
static bridge_queue_packet_t m_queue[NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH];
static uint8_t m_head;
static uint8_t m_tail;
static uint8_t m_count;

static uint16_t m_received;
static uint16_t m_valid;
static uint16_t m_invalid;
static uint16_t m_dropped;
static uint8_t m_last_error;

static void reset_queue(void)
{
    m_head = 0U;
    m_tail = 0U;
    m_count = 0U;
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

    /*
     * B3.1 deliberately accepts unknown transport message types.
     * B4.2 therefore preserves any core-valid non-value frame.  Known
     * CAP_REPORT/CAP_EVENT messages additionally pass the B3.2 parser.
     */
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

bool nrfclaw_ninalink_bridge_start(void)
{
    if (m_active)
        return true;

    reset_queue();
    m_received = 0U;
    m_valid = 0U;
    m_invalid = 0U;
    m_dropped = 0U;
    m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;

    if (!nrfclaw_lora_diag_stream_start())
        return false;

    m_active = true;
    SEGGER_RTT_WriteString(0, "NINALINK B4.2 BRIDGE: RX active\r\n");
    return true;
}

void nrfclaw_ninalink_bridge_stop(void)
{
    if (m_active)
        (void)nrfclaw_lora_cancel_receive();

    m_active = false;
    reset_queue();
    SEGGER_RTT_WriteString(0, "NINALINK B4.2 BRIDGE: RX stopped\r\n");
}

void nrfclaw_ninalink_bridge_process(void)
{
    uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t len;
    int16_t rssi_x2;
    int16_t snr_x4;

    if (!m_active)
        return;

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

        m_received++;

        st = nrfclaw_ninalink_decode(wire, len, &frame);
        if (st != NRFCLAW_NINALINK_OK) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_CORE;
            SEGGER_RTT_printf(
                0,
                "NINALINK B4.2 BRIDGE: reject core status=%u len=%u\r\n",
                (unsigned)st,
                (unsigned)len);
            continue;
        }

        if (!validate_semantic(&frame)) {
            m_invalid++;
            m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_SEMANTIC;
            SEGGER_RTT_printf(
                0,
                "NINALINK B4.2 BRIDGE: reject semantic type=0x%02X seq=%u\r\n",
                (unsigned)frame.message_type,
                (unsigned)frame.sequence);
            continue;
        }

        m_valid++;
        m_last_error = NRFCLAW_NINALINK_BRIDGE_ERR_NONE;
        queue_validated(wire, len, rssi_x2, snr_x4);

        SEGGER_RTT_printf(
            0,
            "NINALINK B4.2 BRIDGE: valid type=0x%02X node=0x%08lX seq=%u len=%u\r\n",
            (unsigned)frame.message_type,
            (unsigned long)frame.node_id,
            (unsigned)frame.sequence,
            (unsigned)len);
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
    if (!out)
        return;

    out->active = m_active;
    out->queued = m_count;
    out->received = m_received;
    out->valid = m_valid;
    out->invalid = m_invalid;
    out->dropped = m_dropped;
    out->radio_dropped = nrfclaw_lora_diag_stream_dropped();
    out->last_error = m_last_error;
}
