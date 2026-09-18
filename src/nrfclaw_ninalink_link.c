#include "nrfclaw_ninalink_link.h"

#include "app_timer.h"
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
#define LINK_MAX_ATTEMPTS       5U
#define LINK_MAX_BACKOFF_MS     4000U

APP_TIMER_DEF(m_retry_timer);

static bool m_retry_timer_initialized;
static volatile bool m_retry_due;
static bool m_started;

static uint8_t m_wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
static uint8_t m_wire_len;

static uint16_t m_next_sequence = 1U;
static uint16_t m_ack_window_ms = 600U;
static uint16_t m_pending_sequence;
static uint16_t m_pending_network_id;
static uint16_t m_base_backoff_ms;

static uint16_t m_acked_total;
static uint16_t m_timeout_total;
static uint16_t m_retry_total;

static nrfclaw_ninalink_link_status_t m_status;

static void retry_timer_handler(void *context)
{
    (void)context;
    m_retry_due = true;
}

static bool ensure_retry_timer(void)
{
    if (m_retry_timer_initialized)
        return true;

    if (app_timer_create(
            &m_retry_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            retry_timer_handler) != NRF_SUCCESS)
        return false;

    m_retry_timer_initialized = true;
    return true;
}

static void refresh_totals(void)
{
    m_status.acked_count = m_acked_total;
    m_status.timeout_count = m_timeout_total;
    m_status.retry_count = m_retry_total;
}

static void set_done(uint8_t result)
{
    m_status.state = NRFCLAW_NINALINK_LINK_DONE;
    m_status.result = result;
    refresh_totals();
}

static bool build_report(uint16_t seq)
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
            m_wire,
            sizeof(m_wire),
            &m_wire_len) != NRFCLAW_NINALINK_OK) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL;
        return false;
    }

    return true;
}

static bool submit_attempt(bool retry)
{
    if (!nrfclaw_lora_idle())
        return false;

    if (!nrfclaw_lora_send_async(m_wire, m_wire_len))
        return false;

    m_status.attempts++;

    if (retry) {
        m_retry_total++;
        refresh_totals();
    }

    m_status.state = NRFCLAW_NINALINK_LINK_WAIT_TX;
    m_status.result = NRFCLAW_NINALINK_LINK_RESULT_NONE;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.4 NODE: TX seq=%u attempt=%u/%u retry=%u\r\n",
        (unsigned)m_pending_sequence,
        (unsigned)m_status.attempts,
        (unsigned)m_status.max_attempts,
        retry ? 1U : 0U);

    return true;
}

static bool schedule_retry(void)
{
    uint32_t delay;
    uint8_t retry_index;

    if (m_status.attempts >= m_status.max_attempts)
        return false;

    if (!ensure_retry_timer()) {
        set_done(NRFCLAW_NINALINK_LINK_RESULT_RETRY_TIMER_FAIL);
        m_started = false;
        return false;
    }

    retry_index = (uint8_t)(m_status.attempts - 1U);
    delay = (uint32_t)m_base_backoff_ms << retry_index;
    if (delay > LINK_MAX_BACKOFF_MS)
        delay = LINK_MAX_BACKOFF_MS;

    m_status.last_backoff_ms = (uint16_t)delay;
    m_retry_due = false;

    if (app_timer_start(
            m_retry_timer,
            APP_TIMER_TICKS(delay),
            NULL) != NRF_SUCCESS) {
        set_done(NRFCLAW_NINALINK_LINK_RESULT_RETRY_TIMER_FAIL);
        m_started = false;
        return false;
    }

    m_status.state = NRFCLAW_NINALINK_LINK_BACKOFF;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.4 NODE: retry seq=%u backoff=%lu ms next_attempt=%u\r\n",
        (unsigned)m_pending_sequence,
        (unsigned long)delay,
        (unsigned)(m_status.attempts + 1U));

    return true;
}

bool nrfclaw_ninalink_link_start_reliable(uint16_t ack_window_ms,
                                          uint8_t max_attempts,
                                          uint16_t base_backoff_ms)
{
    uint16_t seq;

    if (ack_window_ms < LINK_ACK_MIN_WINDOW_MS ||
        ack_window_ms > LINK_ACK_MAX_WINDOW_MS)
        return false;

    if (max_attempts == 0U || max_attempts > LINK_MAX_ATTEMPTS)
        return false;

    if (max_attempts > 1U &&
        (base_backoff_ms == 0U || base_backoff_ms > LINK_MAX_BACKOFF_MS))
        return false;

    if (!nrfclaw_lora_idle())
        return false;

    if (max_attempts > 1U && !ensure_retry_timer())
        return false;

    seq = m_next_sequence;

    memset(&m_status, 0, sizeof(m_status));
    m_status.state = NRFCLAW_NINALINK_LINK_IDLE;
    m_status.sequence = seq;
    m_status.max_attempts = max_attempts;
    m_status.base_backoff_ms = base_backoff_ms;
    refresh_totals();

    m_ack_window_ms = ack_window_ms;
    m_pending_sequence = seq;
    m_pending_network_id = LINK_NETWORK_ID_DEFAULT;
    m_base_backoff_ms = base_backoff_ms;

    if (!build_report(seq)) {
        set_done(m_status.result);
        return false;
    }

    m_status.tx_len = m_wire_len;

    if (!submit_attempt(false)) {
        set_done(NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL);
        return false;
    }

    m_next_sequence++;
    m_started = true;
    return true;
}

bool nrfclaw_ninalink_link_start(uint16_t ack_window_ms)
{
    return nrfclaw_ninalink_link_start_reliable(ack_window_ms, 1U, 0U);
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

    return network_id == m_pending_network_id &&
           ack_seq == m_pending_sequence &&
           wire[15] == 0U;
}

void nrfclaw_ninalink_link_process(void)
{
    if (!m_started)
        return;

    if (m_status.state == NRFCLAW_NINALINK_LINK_BACKOFF) {
        if (!m_retry_due)
            return;

        if (!nrfclaw_lora_idle())
            return;

        m_retry_due = false;

        if (!submit_attempt(true)) {
            set_done(NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL);
            m_started = false;
        }
        return;
    }

    if (m_status.state == NRFCLAW_NINALINK_LINK_WAIT_TX) {
        if (!nrfclaw_lora_idle())
            return;

        if (!nrfclaw_lora_receive_window_async(m_ack_window_ms)) {
            set_done(NRFCLAW_NINALINK_LINK_RESULT_RX_FAIL);
            m_started = false;
            return;
        }

        m_status.state = NRFCLAW_NINALINK_LINK_WAIT_ACK;
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
                m_acked_total++;
                set_done(NRFCLAW_NINALINK_LINK_RESULT_ACKED);
                m_started = false;
                return;
            }

            set_done(NRFCLAW_NINALINK_LINK_RESULT_BAD_ACK);
            m_started = false;
            return;
        }

        if (!nrfclaw_lora_rx_active()) {
            m_timeout_total++;
            refresh_totals();

            if (m_status.attempts < m_status.max_attempts) {
                (void)schedule_retry();
                return;
            }

            set_done(NRFCLAW_NINALINK_LINK_RESULT_TIMEOUT);
            m_started = false;
        }
    }
}

void nrfclaw_ninalink_link_get_status(nrfclaw_ninalink_link_status_t *out)
{
    if (!out)
        return;

    refresh_totals();
    *out = m_status;
}
