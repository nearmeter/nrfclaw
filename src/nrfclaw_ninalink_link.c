#include "nrfclaw_ninalink_link.h"

#include "app_timer.h"
#include "nrf.h"
#include "SEGGER_RTT.h"

#include "nrfclaw_capability.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink.h"
#include "nrfclaw_ninalink_msg.h"
#include "nrfclaw_ninalink_command.h"
#include "nrfclaw_ninalink_command_registry.h"
#include "nrfclaw_tracking.h"

#include <string.h>

#define LINK_NETWORK_ID_DEFAULT 0x0000U
#define LINK_ACK_MIN_WINDOW_MS  100U
#define LINK_ACK_MAX_WINDOW_MS  4000U
#define LINK_MAX_ATTEMPTS       5U
#define LINK_MAX_BACKOFF_MS     4000U

#define APP_CAP_TRACKING_ACTIVE 0x0401U
#define APP_TYPE_BOOL           0x01U
#define APP_CAP_SET_PAYLOAD_LEN 7U
#define APP_CAP_SET_FRAME_LEN   22U
#define APP_ACK_FRAME_LEN       18U

#define COMMAND_ECHO_U32        0x0001U
#define COMMAND_PAYLOAD_BASE    5U
#define COMMAND_RESULT_BASE     4U

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

/* B4.5 application-downlink replay protection/result cache. */
static bool m_app_last_valid;
static uint16_t m_app_last_sequence;
static uint8_t m_app_last_result;
static uint16_t m_app_applied_count;
static uint16_t m_app_duplicate_count;

/* Generic result frame buffer used by CAP_SET and COMMAND. */
static uint8_t m_result_wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
static uint8_t m_result_len;

/* B4.7 COMMAND replay protection/result cache. */
static bool m_cmd_last_valid;
static uint16_t m_cmd_last_sequence;
static uint16_t m_cmd_last_id;
static uint8_t m_cmd_last_result;
static uint8_t m_cmd_last_result_len;
static uint8_t m_cmd_last_result_data[NRFCLAW_NINALINK_COMMAND_RESULT_MAX];
static uint16_t m_cmd_executed_count;
static uint16_t m_cmd_duplicate_count;

/* B4.6/B4.7 deterministic result loss injection. */
static bool m_app_drop_next_result;
static uint16_t m_app_result_drop_count;

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

static void build_result_ack(uint16_t network_id,
                             uint16_t command_seq,
                             uint8_t result)
{
    uint16_t crc;

    memset(m_result_wire, 0, sizeof(m_result_wire));
    m_result_wire[0] = 0x4EU;
    m_result_wire[1] = 0x01U;
    m_result_wire[2] = 0x00U;
    m_result_wire[3] = NRFCLAW_NINALINK_MSG_ACK;
    put_u16_le(&m_result_wire[4], network_id);
    m_result_wire[6] = 3U;
    put_u32_le(&m_result_wire[7], NRF_FICR->DEVICEID[0]);
    put_u16_le(&m_result_wire[11], command_seq);
    put_u16_le(&m_result_wire[13], command_seq);
    m_result_wire[15] = result;
    crc = crc16_ccitt_false(m_result_wire, 16U);
    put_u16_le(&m_result_wire[16], crc);
    m_result_len = APP_ACK_FRAME_LEN;
}

static void build_command_result(uint16_t network_id,
                                 uint16_t command_seq,
                                 uint8_t result,
                                 const uint8_t *data,
                                 uint8_t data_len)
{
    uint8_t payload_len;
    uint8_t crc_off;
    uint16_t crc;

    if (data_len > NRFCLAW_NINALINK_COMMAND_RESULT_MAX)
        data_len = NRFCLAW_NINALINK_COMMAND_RESULT_MAX;

    payload_len = (uint8_t)(COMMAND_RESULT_BASE + data_len);
    crc_off = (uint8_t)(13U + payload_len);

    memset(m_result_wire, 0, sizeof(m_result_wire));
    m_result_wire[0] = 0x4EU;
    m_result_wire[1] = 0x01U;
    m_result_wire[2] = 0x00U;
    m_result_wire[3] = NRFCLAW_NINALINK_MSG_COMMAND_RESULT;
    put_u16_le(&m_result_wire[4], network_id);
    m_result_wire[6] = payload_len;
    put_u32_le(&m_result_wire[7], NRF_FICR->DEVICEID[0]);
    put_u16_le(&m_result_wire[11], command_seq);

    put_u16_le(&m_result_wire[13], command_seq);
    m_result_wire[15] = result;
    m_result_wire[16] = data_len;
    if (data_len && data)
        memcpy(&m_result_wire[17], data, data_len);

    crc = crc16_ccitt_false(m_result_wire, crc_off);
    put_u16_le(&m_result_wire[crc_off], crc);
    m_result_len = (uint8_t)(crc_off + 2U);
}

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

static bool accept_normal_ack(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint16_t ack_seq;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (len != APP_ACK_FRAME_LEN ||
        wire[3] != NRFCLAW_NINALINK_MSG_ACK ||
        wire[6] != 3U)
        return false;

    network_id = get_u16_le(&wire[4]);
    ack_seq = get_u16_le(&wire[13]);

    return network_id == m_pending_network_id &&
           ack_seq == m_pending_sequence &&
           wire[15] == 0U;
}

static uint8_t apply_cap_set(uint16_t cap_id,
                             uint8_t channel,
                             uint8_t type,
                             uint8_t value)
{
    if (cap_id != APP_CAP_TRACKING_ACTIVE || channel != 0U)
        return NRFCLAW_NINALINK_APP_UNSUPPORTED_CAP;

    if (type != APP_TYPE_BOOL)
        return NRFCLAW_NINALINK_APP_BAD_TYPE;

    if (value > 1U)
        return NRFCLAW_NINALINK_APP_BAD_VALUE;

    if (value == 0U) {
        nrfclaw_tracking_stop();
        return nrfclaw_tracking_active()
            ? NRFCLAW_NINALINK_APP_APPLY_FAILED
            : NRFCLAW_NINALINK_APP_OK;
    }

    if (nrfclaw_tracking_active())
        return NRFCLAW_NINALINK_APP_OK;

    return nrfclaw_tracking_start() == NRFCLAW_TRACKING_OK
        ? NRFCLAW_NINALINK_APP_OK
        : NRFCLAW_NINALINK_APP_APPLY_FAILED;
}

static bool accept_cap_set(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint32_t target_node;
    uint16_t command_seq;
    uint16_t reply_to_seq;
    uint16_t cap_id;
    uint8_t channel;
    uint8_t type;
    uint8_t value;
    uint8_t result;

    if (len != APP_CAP_SET_FRAME_LEN)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_CAP_SET ||
        wire[6] != APP_CAP_SET_PAYLOAD_LEN)
        return false;

    network_id = get_u16_le(&wire[4]);
    target_node = get_u32_le(&wire[7]);
    command_seq = get_u16_le(&wire[11]);
    reply_to_seq = get_u16_le(&wire[13]);

    if (network_id != m_pending_network_id ||
        target_node != NRF_FICR->DEVICEID[0] ||
        reply_to_seq != m_pending_sequence)
        return false;

    cap_id = get_u16_le(&wire[15]);
    channel = wire[17];
    type = wire[18];
    value = wire[19];

    if (m_app_last_valid && command_seq == m_app_last_sequence) {
        result = m_app_last_result;
        m_app_duplicate_count++;
    } else {
        result = apply_cap_set(cap_id, channel, type, value);
        m_app_last_valid = true;
        m_app_last_sequence = command_seq;
        m_app_last_result = result;
        m_app_applied_count++;
    }

    build_result_ack(network_id, command_seq, result);

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.5 NODE: CAP_SET seq=%u reply_to=%u cap=0x%04X value=%u result=%u dup=%u\r\n",
        (unsigned)command_seq,
        (unsigned)reply_to_seq,
        (unsigned)cap_id,
        (unsigned)value,
        (unsigned)result,
        (unsigned)m_app_duplicate_count);

    return true;
}

static uint8_t execute_command(uint16_t command_id,
                               const uint8_t *args,
                               uint8_t arg_len,
                               uint8_t *result,
                               uint8_t *result_len)
{
    nrfclaw_ninalink_command_context_t context;

    context.node_id = NRF_FICR->DEVICEID[0];
    context.tracking_active = nrfclaw_tracking_active();

    return nrfclaw_ninalink_command_registry_execute(
        &context,
        command_id,
        args,
        arg_len,
        result,
        result_len);
}

static bool accept_command(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint32_t target_node;
    uint16_t command_seq;
    uint16_t reply_to_seq;
    uint16_t command_id;
    uint8_t arg_len;
    uint8_t payload_len;
    uint8_t result;
    uint8_t result_len = 0U;
    uint8_t result_data[NRFCLAW_NINALINK_COMMAND_RESULT_MAX];

    if (len < 20U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_COMMAND)
        return false;

    payload_len = wire[6];
    if (payload_len < COMMAND_PAYLOAD_BASE)
        return false;

    arg_len = wire[17];
    if ((uint8_t)(COMMAND_PAYLOAD_BASE + arg_len) != payload_len)
        return false;

    if ((uint16_t)13U + payload_len + 2U != len)
        return false;

    network_id = get_u16_le(&wire[4]);
    target_node = get_u32_le(&wire[7]);
    command_seq = get_u16_le(&wire[11]);
    reply_to_seq = get_u16_le(&wire[13]);
    command_id = get_u16_le(&wire[15]);

    if (network_id != m_pending_network_id ||
        target_node != NRF_FICR->DEVICEID[0] ||
        reply_to_seq != m_pending_sequence)
        return false;

    if (m_cmd_last_valid && command_seq == m_cmd_last_sequence) {
        result = m_cmd_last_result;
        result_len = m_cmd_last_result_len;
        memcpy(result_data, m_cmd_last_result_data, result_len);
        m_cmd_duplicate_count++;
    } else {
        memset(result_data, 0, sizeof(result_data));
        result = execute_command(
            command_id,
            &wire[18],
            arg_len,
            result_data,
            &result_len);

        m_cmd_last_valid = true;
        m_cmd_last_sequence = command_seq;
        m_cmd_last_id = command_id;
        m_cmd_last_result = result;
        m_cmd_last_result_len = result_len;
        memcpy(m_cmd_last_result_data, result_data, result_len);
        if (result != NRFCLAW_NINALINK_COMMAND_UNSUPPORTED &&
            result != NRFCLAW_NINALINK_COMMAND_BAD_ARGS)
            m_cmd_executed_count++;
    }

    build_command_result(
        network_id,
        command_seq,
        result,
        result_data,
        result_len);

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.7 NODE: COMMAND seq=%u id=0x%04X args=%u result=%u rlen=%u dup=%u\r\n",
        (unsigned)command_seq,
        (unsigned)command_id,
        (unsigned)arg_len,
        (unsigned)result,
        (unsigned)result_len,
        (unsigned)m_cmd_duplicate_count);

    return true;
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

            if (accept_normal_ack(wire, len)) {
                m_acked_total++;
                set_done(NRFCLAW_NINALINK_LINK_RESULT_ACKED);
                m_started = false;
                return;
            }

            /*
             * CAP_SET carries reply_to_seq, so receiving a valid command is
             * also proof that the bridge received this uplink. The node sends
             * an application-result ACK for the command sequence.
             */
            if (accept_cap_set(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
                return;
            }

            if (accept_command(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
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
        return;
    }

    if (m_status.state == NRFCLAW_NINALINK_LINK_APP_RESULT_ARM) {
        if (!nrfclaw_lora_idle())
            return;

        /*
         * B4.6/B4.7 lab gate: CAP_SET/COMMAND has already acknowledged the
         * triggering uplink through reply_to_seq. Suppressing the result
         * frame must therefore NOT cause the node to retry that uplink.
         */
        if (m_app_drop_next_result) {
            m_app_drop_next_result = false;
            m_app_result_drop_count++;
            m_acked_total++;
            set_done(NRFCLAW_NINALINK_LINK_RESULT_ACKED);
            m_started = false;

            SEGGER_RTT_printf(
                0,
                "NINALINK B4.7 NODE: TEST drop result count=%u\r\n",
                (unsigned)m_app_result_drop_count);
            return;
        }

        if (!nrfclaw_lora_send_async(
                m_result_wire,
                m_result_len)) {
            set_done(NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL);
            m_started = false;
            return;
        }

        m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_TX;
        return;
    }

    if (m_status.state == NRFCLAW_NINALINK_LINK_APP_RESULT_TX) {
        if (!nrfclaw_lora_idle())
            return;

        m_acked_total++;
        set_done(NRFCLAW_NINALINK_LINK_RESULT_ACKED);
        m_started = false;
    }
}

void nrfclaw_ninalink_link_get_status(nrfclaw_ninalink_link_status_t *out)
{
    if (!out)
        return;

    refresh_totals();
    *out = m_status;
}

void nrfclaw_ninalink_link_get_app_status(nrfclaw_ninalink_app_status_t *out)
{
    if (!out)
        return;

    out->valid = m_app_last_valid;
    out->sequence = m_app_last_sequence;
    out->result = m_app_last_result;
    out->applied_count = m_app_applied_count;
    out->duplicate_count = m_app_duplicate_count;
    out->tracking_active = nrfclaw_tracking_active();
}

void nrfclaw_ninalink_link_drop_next_app_result(void)
{
    m_app_drop_next_result = true;
}

bool nrfclaw_ninalink_link_app_result_drop_armed(void)
{
    return m_app_drop_next_result;
}

uint16_t nrfclaw_ninalink_link_app_result_drop_count(void)
{
    return m_app_result_drop_count;
}

void nrfclaw_ninalink_link_get_command_status(
    nrfclaw_ninalink_command_status_t *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->valid = m_cmd_last_valid;
    out->sequence = m_cmd_last_sequence;
    out->command_id = m_cmd_last_id;
    out->result = m_cmd_last_result;
    out->result_len = m_cmd_last_result_len;
    if (m_cmd_last_result_len)
        memcpy(
            out->result_data,
            m_cmd_last_result_data,
            m_cmd_last_result_len);
    out->executed_count = m_cmd_executed_count;
    out->duplicate_count = m_cmd_duplicate_count;
}
