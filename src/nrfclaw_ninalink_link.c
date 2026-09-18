#include "nrfclaw_ninalink_link.h"

#include "nrf.h"
#include "SEGGER_RTT.h"

#include "nrfclaw_capability.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink.h"
#include "nrfclaw_ninalink_msg.h"

#include <string.h>

#define LINK_NETWORK_ID_DEFAULT 0x0000U
#define LINK_ACK_MIN_WINDOW_MS  100U
#define LINK_ACK_MAX_WINDOW_MS  4000U

static nrfclaw_ninalink_link_status_t m_status;
static uint16_t m_next_sequence = 1U;
static uint16_t m_ack_window_ms = 600U;
static uint16_t m_pending_sequence;
static uint16_t m_pending_network_id;
static bool m_started;

static void set_done(uint8_t result)
{
    m_status.state = NRFCLAW_NINALINK_LINK_DONE;
    m_status.result = result;
}

static bool build_report(uint8_t *wire, uint8_t *wire_len, uint16_t seq)
{
    nrfclaw_ninalink_value_entry_t entries[2];
    nrfclaw_capability_value_t value;
    nrfclaw_ninalink_frame_t frame;
    uint8_t count = 0U;

    memset(entries, 0, sizeof(entries));

    if (nrfclaw_capability_read_current(
            NRFCLAW_SEMCAP_BATTERY_VOLTAGE, 0U, &value)) {
        entries[count].capability_id = NRFCLAW_SEMCAP_BATTERY_VOLTAGE;
        entries[count].channel = 0U;
        entries[count].value = value;
        count++;
    }

    if (nrfclaw_capability_read_current(
            NRFCLAW_SEMCAP_TEMPERATURE, 0U, &value)) {
        entries[count].capability_id = NRFCLAW_SEMCAP_TEMPERATURE;
        entries[count].channel = 0U;
        entries[count].value = value;
        count++;
    }

    if (count == 0U) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_NO_DATA;
        return false;
    }

    if (nrfclaw_ninalink_build_values(
            &frame,
            NRFCLAW_NINALINK_MSG_CAP_REPORT,
            LINK_NETWORK_ID_DEFAULT,
            NRF_FICR->DEVICEID[0],
            seq,
            true,
            entries,
            count) != NRFCLAW_NINALINK_MSG_OK) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL;
        return false;
    }

    if (nrfclaw_ninalink_encode(
            &frame,
            wire,
            NRFCLAW_NINALINK_MAX_FRAME_SIZE,
            wire_len) != NRFCLAW_NINALINK_OK) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL;
        return false;
    }

    return true;
}

bool nrfclaw_ninalink_link_start(uint16_t ack_window_ms)
{
    uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t wire_len = 0U;
    uint16_t seq;
    uint16_t acked_count = m_status.acked_count;
    uint16_t timeout_count = m_status.timeout_count;

    if (ack_window_ms < LINK_ACK_MIN_WINDOW_MS ||
        ack_window_ms > LINK_ACK_MAX_WINDOW_MS)
        return false;

    if (!nrfclaw_lora_idle())
        return false;

    seq = m_next_sequence;

    memset(&m_status, 0, sizeof(m_status));
    m_status.acked_count = acked_count;
    m_status.timeout_count = timeout_count;
    m_status.state = NRFCLAW_NINALINK_LINK_IDLE;
    m_status.sequence = seq;
    m_ack_window_ms = ack_window_ms;
    m_pending_sequence = seq;
    m_pending_network_id = LINK_NETWORK_ID_DEFAULT;

    if (!build_report(wire, &wire_len, seq)) {
        set_done(m_status.result);
        return false;
    }

    m_status.tx_len = wire_len;

    if (!nrfclaw_lora_send_async(wire, wire_len)) {
        set_done(NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL);
        return false;
    }

    m_next_sequence++;
    m_status.state = NRFCLAW_NINALINK_LINK_WAIT_TX;
    m_status.result = NRFCLAW_NINALINK_LINK_RESULT_NONE;
    m_started = true;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.3 NODE: TX ACK_REQ seq=%u len=%u window=%u ms\r\n",
        (unsigned)seq,
        (unsigned)wire_len,
        (unsigned)ack_window_ms);

    return true;
}

static bool accept_ack(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint16_t ack_seq;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (len != 18U ||
        wire[3] != NRFCLAW_NINALINK_MSG_ACK ||
        wire[6] != 3U)
        return false;

    network_id = (uint16_t)wire[4] | ((uint16_t)wire[5] << 8);
    ack_seq = (uint16_t)wire[13] | ((uint16_t)wire[14] << 8);

    if (network_id != m_pending_network_id ||
        ack_seq != m_pending_sequence ||
        wire[15] != 0U)
        return false;

    return true;
}

void nrfclaw_ninalink_link_process(void)
{
    if (!m_started)
        return;

    if (m_status.state == NRFCLAW_NINALINK_LINK_WAIT_TX) {
        if (!nrfclaw_lora_idle())
            return;

        if (!nrfclaw_lora_receive_window_async(m_ack_window_ms)) {
            set_done(NRFCLAW_NINALINK_LINK_RESULT_RX_FAIL);
            m_started = false;
            return;
        }

        m_status.state = NRFCLAW_NINALINK_LINK_WAIT_ACK;
        SEGGER_RTT_printf(
            0,
            "NINALINK B4.3 NODE: ACK RX open seq=%u\r\n",
            (unsigned)m_pending_sequence);
        return;
    }

    if (m_status.state == NRFCLAW_NINALINK_LINK_WAIT_ACK) {
        if (nrfclaw_lora_rx_ready()) {
            uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
            uint8_t len = 0U;
            int16_t rssi = 0;
            int16_t snr = 0;

            if (!nrfclaw_lora_take_rx(wire, &len, sizeof(wire))) {
                set_done(NRFCLAW_NINALINK_LINK_RESULT_BAD_ACK);
                m_started = false;
                return;
            }

            m_status.ack_len = len;
            if (nrfclaw_lora_get_last_packet_status(&rssi, &snr)) {
                m_status.ack_rssi_x2 = rssi;
                m_status.ack_snr_x4 = snr;
            }

            if (accept_ack(wire, len)) {
                m_status.acked_count++;
                set_done(NRFCLAW_NINALINK_LINK_RESULT_ACKED);
                SEGGER_RTT_printf(
                    0,
                    "NINALINK B4.3 NODE: ACKED seq=%u RSSI_x2=%d SNR_x4=%d\r\n",
                    (unsigned)m_pending_sequence,
                    (int)m_status.ack_rssi_x2,
                    (int)m_status.ack_snr_x4);
            } else {
                set_done(NRFCLAW_NINALINK_LINK_RESULT_BAD_ACK);
            }

            m_started = false;
            return;
        }

        if (!nrfclaw_lora_rx_active()) {
            m_status.timeout_count++;
            set_done(NRFCLAW_NINALINK_LINK_RESULT_TIMEOUT);
            m_started = false;
            SEGGER_RTT_printf(
                0,
                "NINALINK B4.3 NODE: ACK timeout seq=%u\r\n",
                (unsigned)m_pending_sequence);
        }
    }
}

void nrfclaw_ninalink_link_get_status(nrfclaw_ninalink_link_status_t *out)
{
    if (out)
        *out = m_status;
}
