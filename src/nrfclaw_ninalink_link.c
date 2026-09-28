#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_ninalink_report_packer.h"

#include "app_timer.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "nrfclaw_capability.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink.h"
#include "nrfclaw_ninalink_msg.h"
#include "nrfclaw_ninalink_network.h"
#include "nrfclaw_ninalink_command.h"
#include "nrfclaw_ninalink_command_registry.h"
#include "nrfclaw_ninalink_command_discovery.h"
#include "nrfclaw_ninalink_capability_discovery.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_vm_semantic_state.h"

#include <string.h>

#define LINK_ACK_MIN_WINDOW_MS  100U
#define LINK_ACK_MAX_WINDOW_MS  4000U
#define LINK_MAX_ATTEMPTS       5U
#define LINK_MAX_BACKOFF_MS     4000U

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

static bool m_session_valid;
static uint32_t m_session_id;
static uint16_t m_session_generation;


static nrfclaw_ninalink_link_status_t m_status;

/* B4.5 application-downlink replay protection/result cache. */
static bool m_app_last_valid;
static uint16_t m_app_last_sequence;
static uint8_t m_app_last_result;

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

/* B4.9 COMMAND registry discovery status/replay observation. */
static bool m_disc_last_valid;
static uint16_t m_disc_last_sequence;
static uint8_t m_disc_last_start;
static uint8_t m_disc_last_total;
static uint8_t m_disc_last_count;
static uint16_t m_disc_served_count;
static uint16_t m_disc_duplicate_count;

/* B4.10 semantic capability discovery status/replay observation. */
static bool m_capdisc_last_valid;
static uint16_t m_capdisc_last_sequence;
static uint8_t m_capdisc_last_page;
static uint8_t m_capdisc_last_count;
static bool m_capdisc_last_more;
static uint16_t m_capdisc_served_count;
static uint16_t m_capdisc_duplicate_count;

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

static bool build_commands_response(uint16_t network_id,
                                    uint16_t request_seq,
                                    uint8_t start_index)
{
    uint8_t payload[46U];
    uint8_t payload_len = 0U;
    uint8_t crc_off;
    uint16_t crc;

    if (!nrfclaw_ninalink_command_discovery_build_page(
            start_index,
            payload,
            sizeof(payload),
            &payload_len))
        return false;

    crc_off = (uint8_t)(13U + payload_len);

    memset(m_result_wire, 0, sizeof(m_result_wire));
    m_result_wire[0] = 0x4EU;
    m_result_wire[1] = 0x01U;
    m_result_wire[2] = 0x00U;
    m_result_wire[3] = NRFCLAW_NINALINK_MSG_COMMANDS_RESPONSE;
    put_u16_le(&m_result_wire[4], network_id);
    m_result_wire[6] = payload_len;
    put_u32_le(&m_result_wire[7], NRF_FICR->DEVICEID[0]);
    put_u16_le(&m_result_wire[11], request_seq);
    memcpy(&m_result_wire[13], payload, payload_len);

    crc = crc16_ccitt_false(m_result_wire, crc_off);
    put_u16_le(&m_result_wire[crc_off], crc);
    m_result_len = (uint8_t)(crc_off + 2U);

    return true;
}


#define LINK_APP_RESULT_GUARD_MS 60U

APP_TIMER_DEF(m_app_result_guard_timer);

static bool m_app_result_guard_timer_initialized;
static volatile bool m_app_result_guard_due;
static bool m_app_result_guard_armed;

static void app_result_guard_timer_handler(void *context)
{
    (void)context;
    m_app_result_guard_due = true;
}

static bool ensure_app_result_guard_timer(void)
{
    if (m_app_result_guard_timer_initialized)
        return true;

    if (app_timer_create(
            &m_app_result_guard_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            app_result_guard_timer_handler) != NRF_SUCCESS)
        return false;

    m_app_result_guard_timer_initialized = true;
    return true;
}

static bool app_result_guard_ready(void)
{
    if (!m_app_result_guard_armed) {
        if (!ensure_app_result_guard_timer())
            return false;

        m_app_result_guard_due = false;
        if (app_timer_start(
                m_app_result_guard_timer,
                APP_TIMER_TICKS(LINK_APP_RESULT_GUARD_MS),
                NULL) != NRF_SUCCESS)
            return false;

        m_app_result_guard_armed = true;
        return false;
    }

    if (!m_app_result_guard_due)
        return false;

    m_app_result_guard_due = false;
    m_app_result_guard_armed = false;
    return true;
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

static bool session_try_init(void)
{
    uint8_t available = 0U;
    uint8_t bytes[4];
    uint32_t sid;

    if (m_session_valid)
        return true;

    if (sd_rand_application_bytes_available_get(&available) != NRF_SUCCESS ||
        available < sizeof(bytes))
        return false;

    if (sd_rand_application_vector_get(bytes, sizeof(bytes)) != NRF_SUCCESS)
        return false;

    sid = ((uint32_t)bytes[0]) |
          ((uint32_t)bytes[1] << 8) |
          ((uint32_t)bytes[2] << 16) |
          ((uint32_t)bytes[3] << 24);

    if (sid == 0U || sid == 0xFFFFFFFFUL)
        sid ^= NRF_FICR->DEVICEID[0] ^ 0xA53D53D0UL;
    if (sid == 0U || sid == 0xFFFFFFFFUL)
        sid = 1U;

    m_session_id = sid;
    m_session_valid = true;
    m_session_generation = m_session_generation == 0xFFFFU
        ? m_session_generation
        : (uint16_t)(m_session_generation + 1U);
    return true;
}

static nrfclaw_ninalink_msg_status_t build_values_with_session(
    nrfclaw_ninalink_frame_t *frame,
    uint8_t message_type,
    uint16_t seq,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t count)
{
    nrfclaw_ninalink_value_entry_t
        work[NRFCLAW_NINALINK_MAX_VALUE_ENTRIES];

    if (!frame || !entries || count == 0U ||
        count >= NRFCLAW_NINALINK_MAX_VALUE_ENTRIES)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (!session_try_init())
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    memcpy(work, entries, (size_t)count * sizeof(work[0]));
    memset(&work[count], 0, sizeof(work[count]));
    work[count].capability_id = NRFCLAW_NINALINK_META_SESSION_ID;
    work[count].channel = 0U;
    work[count].value.type = NRFCLAW_CAP_VALUE_U32;
    work[count].value.v.u32 = m_session_id;

    return nrfclaw_ninalink_build_values(
        frame,
        message_type,
        nrfclaw_ninalink_network_id(),
        NRF_FICR->DEVICEID[0],
        seq,
        true,
        work,
        (uint8_t)(count + 1U));
}

static bool build_report(uint16_t seq)
{
    nrfclaw_ninalink_value_entry_t
        entries[NRFCLAW_NINALINK_MAX_VALUE_ENTRIES - 1U];
    nrfclaw_ninalink_report_pack_status_t pack_status;
    nrfclaw_ninalink_frame_t frame;
    uint8_t count = 0U;

    memset(entries, 0, sizeof(entries));
    memset(&pack_status, 0, sizeof(pack_status));

    if (!nrfclaw_ninalink_report_pack(
            entries,
            (uint8_t)(sizeof(entries) / sizeof(entries[0])),
            &count,
            &pack_status)) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_NO_DATA;
        return false;
    }

    if (build_values_with_session(
            &frame,
            NRFCLAW_NINALINK_MSG_CAP_REPORT,
            seq,
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

static bool build_state_test(uint16_t seq, int16_t temperature_centi)
{
    nrfclaw_ninalink_value_entry_t entry;
    nrfclaw_ninalink_frame_t frame;

    memset(&entry, 0, sizeof(entry));
    entry.capability_id = NRFCLAW_SEMCAP_TEMPERATURE;
    entry.channel = 0U;
    entry.value.type = NRFCLAW_CAP_VALUE_S16;
    entry.value.v.s16 = temperature_centi;

    if (build_values_with_session(
            &frame,
            NRFCLAW_NINALINK_MSG_CAP_REPORT,
            seq,
            &entry,
            1U) != NRFCLAW_NINALINK_MSG_OK) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL;
        return false;
    }

    if (nrfclaw_ninalink_encode(
            &frame, m_wire, sizeof(m_wire), &m_wire_len) !=
            NRFCLAW_NINALINK_OK) {
        m_status.result = NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL;
        return false;
    }

    return true;
}

static bool build_event_value(
    uint16_t seq,
    uint16_t capability_id,
    uint8_t channel,
    const nrfclaw_capability_value_t *value)
{
    nrfclaw_ninalink_value_entry_t entry;
    nrfclaw_ninalink_frame_t frame;

    if (!value)
        return false;

    memset(&entry, 0, sizeof(entry));
    entry.capability_id = capability_id;
    entry.channel = channel;
    entry.value = *value;

    if (build_values_with_session(
            &frame,
            NRFCLAW_NINALINK_MSG_CAP_EVENT,
            seq,
            &entry,
            1U) != NRFCLAW_NINALINK_MSG_OK) {
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
    m_pending_network_id = nrfclaw_ninalink_network_id();
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

bool nrfclaw_ninalink_link_start_state_test(int16_t temperature_centi,
                                            uint16_t forced_sequence,
                                            uint16_t ack_window_ms,
                                            uint8_t max_attempts,
                                            uint16_t base_backoff_ms)
{
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

    memset(&m_status, 0, sizeof(m_status));
    m_status.state = NRFCLAW_NINALINK_LINK_IDLE;
    m_status.sequence = forced_sequence;
    m_status.max_attempts = max_attempts;
    m_status.base_backoff_ms = base_backoff_ms;
    refresh_totals();

    m_ack_window_ms = ack_window_ms;
    m_pending_sequence = forced_sequence;
    m_pending_network_id = nrfclaw_ninalink_network_id();
    m_base_backoff_ms = base_backoff_ms;

    if (!build_state_test(forced_sequence, temperature_centi)) {
        set_done(m_status.result);
        return false;
    }

    m_status.tx_len = m_wire_len;
    if (!submit_attempt(false)) {
        set_done(NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL);
        return false;
    }

    /* Lab-only forced sequence: do not modify production m_next_sequence. */
    m_started = true;
    return true;
}

bool nrfclaw_ninalink_link_start_temperature_mC(
    int32_t temperature_mC,
    uint16_t ack_window_ms,
    uint8_t max_attempts,
    uint16_t base_backoff_ms)
{
    int32_t centi32;
    uint16_t seq;

    centi32 = temperature_mC / 10L;
    if (centi32 < -32768L || centi32 > 32767L)
        return false;

    seq = m_next_sequence;

    if (!nrfclaw_ninalink_link_start_state_test(
            (int16_t)centi32,
            seq,
            ack_window_ms,
            max_attempts,
            base_backoff_ms))
        return false;

    /*
     * One production sequence per logical transaction.
     * Retries reuse the same encoded wire image inside the link engine.
     */
    m_next_sequence++;
    return true;
}

bool nrfclaw_ninalink_link_busy(void)
{
    return m_started;
}

bool nrfclaw_ninalink_link_start_event(
    uint16_t capability_id,
    uint8_t channel,
    uint8_t value_type,
    uint32_t raw_value,
    uint16_t ack_window_ms,
    uint8_t max_attempts,
    uint16_t base_backoff_ms)
{
    nrfclaw_capability_desc_t desc;
    nrfclaw_capability_value_t value;
    uint16_t seq;

    if (m_started)
        return false;

    if (!nrfclaw_capability_descriptor(
            capability_id, channel, &desc))
        return false;

    /*
     * CAP_EVENT transports asynchronous occurrences from EVENT_SOURCE
     * capabilities. Most are semantic EVENTs, but Motion is
     * STATE+EVENT_SOURCE: the occurrence asserts a state-style HA
     * presentation without creating a retained Motion=true cache value.
     */
    if ((desc.kind != NRFCLAW_CAP_KIND_EVENT &&
         desc.kind != NRFCLAW_CAP_KIND_STATE) ||
        (desc.behavior_flags &
         NRFCLAW_CAP_BEHAVIOR_EVENT_SOURCE) == 0U ||
        desc.value_type != value_type)
        return false;

    memset(&value, 0, sizeof(value));
    value.type = value_type;

    switch (value_type) {
        case NRFCLAW_CAP_VALUE_BOOL:
            value.v.boolean = raw_value != 0U;
            break;
        case NRFCLAW_CAP_VALUE_U8:
            if (raw_value > 0xFFU)
                return false;
            value.v.u8 = (uint8_t)raw_value;
            break;
        case NRFCLAW_CAP_VALUE_S8:
            if (raw_value > 0xFFU)
                return false;
            value.v.s8 = (int8_t)(uint8_t)raw_value;
            break;
        case NRFCLAW_CAP_VALUE_U16:
            if (raw_value > 0xFFFFU)
                return false;
            value.v.u16 = (uint16_t)raw_value;
            break;
        case NRFCLAW_CAP_VALUE_S16:
            if (raw_value > 0xFFFFU)
                return false;
            value.v.s16 = (int16_t)(uint16_t)raw_value;
            break;
        case NRFCLAW_CAP_VALUE_U32:
            value.v.u32 = raw_value;
            break;
        case NRFCLAW_CAP_VALUE_S32:
            value.v.s32 = (int32_t)raw_value;
            break;
        case NRFCLAW_CAP_VALUE_ENUM8:
            if (raw_value > 0xFFU)
                return false;
            value.v.u8 = (uint8_t)raw_value;
            break;
        default:
            return false;
    }

    if (ack_window_ms < LINK_ACK_MIN_WINDOW_MS ||
        ack_window_ms > LINK_ACK_MAX_WINDOW_MS)
        return false;

    if (max_attempts == 0U || max_attempts > LINK_MAX_ATTEMPTS)
        return false;

    if (max_attempts > 1U &&
        (base_backoff_ms == 0U ||
         base_backoff_ms > LINK_MAX_BACKOFF_MS))
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
    m_pending_network_id = nrfclaw_ninalink_network_id();
    m_base_backoff_ms = base_backoff_ms;

    if (!build_event_value(
            seq, capability_id, channel, &value)) {
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
    nrfclaw_capability_desc_t desc;
    nrfclaw_capability_write_result_t wr;

    if (!nrfclaw_capability_descriptor(cap_id, channel, &desc) ||
        (desc.behavior_flags & NRFCLAW_CAP_BEHAVIOR_WRITABLE) == 0U)
        return NRFCLAW_NINALINK_APP_UNSUPPORTED_CAP;

    wr = nrfclaw_capability_write(cap_id, channel, type, value);

    switch (wr) {
        case NRFCLAW_CAP_WRITE_OK:
            return NRFCLAW_NINALINK_APP_OK;

        case NRFCLAW_CAP_WRITE_BAD_TYPE:
            return NRFCLAW_NINALINK_APP_BAD_TYPE;

        case NRFCLAW_CAP_WRITE_BAD_VALUE:
            return NRFCLAW_NINALINK_APP_BAD_VALUE;

        case NRFCLAW_CAP_WRITE_UNSUPPORTED:
        case NRFCLAW_CAP_WRITE_NOT_WRITABLE:
            return NRFCLAW_NINALINK_APP_UNSUPPORTED_CAP;

        case NRFCLAW_CAP_WRITE_APPLY_FAILED:
        default:
            return NRFCLAW_NINALINK_APP_APPLY_FAILED;
    }
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
    } else {
        result = apply_cap_set(cap_id, channel, type, value);
        m_app_last_valid = true;
        m_app_last_sequence = command_seq;
        m_app_last_result = result;
    }

    build_result_ack(network_id, command_seq, result);

    ((void)0);

    return true;
}

static uint32_t capability_value_raw(
    const nrfclaw_capability_value_t *value)
{
    switch (value->type) {
        case NRFCLAW_CAP_VALUE_BOOL:
            return value->v.boolean ? 1UL : 0UL;
        case NRFCLAW_CAP_VALUE_U8:
            return value->v.u8;
        case NRFCLAW_CAP_VALUE_S8:
            return (uint32_t)(int32_t)value->v.s8;
        case NRFCLAW_CAP_VALUE_U16:
            return value->v.u16;
        case NRFCLAW_CAP_VALUE_S16:
            return (uint32_t)(int32_t)value->v.s16;
        case NRFCLAW_CAP_VALUE_U32:
            return value->v.u32;
        case NRFCLAW_CAP_VALUE_S32:
            return (uint32_t)value->v.s32;
        case NRFCLAW_CAP_VALUE_ENUM8:
            return value->v.u8;
        default:
            return 0UL;
    }
}

static uint8_t capability_query_adapter(
    uint16_t capability_id,
    uint8_t channel,
    uint8_t *value_type,
    uint32_t *raw_value)
{
    nrfclaw_capability_desc_t desc;
    nrfclaw_capability_value_t value;

    if (!value_type || !raw_value)
        return NRFCLAW_NINALINK_QUERY_ITEM_UNAVAILABLE;

    if (!nrfclaw_capability_descriptor(capability_id, channel, &desc))
        return NRFCLAW_NINALINK_QUERY_ITEM_UNKNOWN;

    if ((desc.behavior_flags & NRFCLAW_CAP_BEHAVIOR_READABLE) == 0U)
        return NRFCLAW_NINALINK_QUERY_ITEM_NOT_READABLE;

    if (!nrfclaw_capability_query_read(capability_id, channel, &value))
        return NRFCLAW_NINALINK_QUERY_ITEM_UNAVAILABLE;

    if (value.type != desc.value_type)
        return NRFCLAW_NINALINK_QUERY_ITEM_UNAVAILABLE;

    *value_type = value.type;
    *raw_value = capability_value_raw(&value);
    return NRFCLAW_NINALINK_QUERY_ITEM_OK;
}

static uint8_t execute_command(uint16_t command_id,
                               const uint8_t *args,
                               uint8_t arg_len,
                               uint8_t *result,
                               uint8_t *result_len)
{
    nrfclaw_ninalink_command_context_t context;

    memset(&context, 0, sizeof(context));

    context.node_id = NRF_FICR->DEVICEID[0];
    context.tracking_active = nrfclaw_tracking_active();
    context.query_read = capability_query_adapter;

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

    ((void)0);

    return true;
}

static bool accept_commands_request(const uint8_t *wire, uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint32_t target_node;
    uint16_t request_seq;
    uint16_t reply_to_seq;
    uint8_t start_index;

    if (len != 18U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_COMMANDS_REQUEST ||
        wire[6] != 3U)
        return false;

    network_id = get_u16_le(&wire[4]);
    target_node = get_u32_le(&wire[7]);
    request_seq = get_u16_le(&wire[11]);
    reply_to_seq = get_u16_le(&wire[13]);
    start_index = wire[15];

    if (network_id != m_pending_network_id ||
        target_node != NRF_FICR->DEVICEID[0] ||
        reply_to_seq != m_pending_sequence)
        return false;

    if (m_disc_last_valid && request_seq == m_disc_last_sequence) {
        if (start_index != m_disc_last_start)
            return false;
        m_disc_duplicate_count++;
    } else {
        m_disc_last_valid = true;
        m_disc_last_sequence = request_seq;
        m_disc_last_start = start_index;
        m_disc_served_count++;
    }

    if (!build_commands_response(network_id, request_seq, start_index))
        return false;

    m_disc_last_total = m_result_wire[15];
    m_disc_last_count = m_result_wire[16];

    ((void)0);

    return true;
}

static bool build_capability_response(uint16_t network_id,
                                      uint16_t request_seq,
                                      uint8_t page_index)
{
    nrfclaw_ninalink_capability_discovery_page_t page;
    nrfclaw_ninalink_frame_t frame;

    if (!nrfclaw_ninalink_capability_discovery_build_page(
            page_index, &page))
        return false;

    if (nrfclaw_ninalink_build_caps_response(
            &frame,
            network_id,
            NRF_FICR->DEVICEID[0],
            request_seq,
            false,
            page.more,
            NRFCLAW_CAPABILITY_REGISTRY_VERSION,
            page_index,
            page.descriptors,
            page.count) != NRFCLAW_NINALINK_MSG_OK)
        return false;

    if (nrfclaw_ninalink_encode(
            &frame,
            m_result_wire,
            sizeof(m_result_wire),
            &m_result_len) != NRFCLAW_NINALINK_OK)
        return false;

    m_capdisc_last_count = page.count;
    m_capdisc_last_more = page.more;
    return true;
}

static bool accept_capability_request(
    const uint8_t *wire,
    uint8_t len)
{
    nrfclaw_ninalink_frame_t frame;
    uint16_t network_id;
    uint32_t target_node;
    uint16_t request_seq;
    uint16_t reply_to_seq;
    uint8_t page_index;

    if (len != 18U)
        return false;

    if (nrfclaw_ninalink_decode(wire, len, &frame) != NRFCLAW_NINALINK_OK)
        return false;

    if (wire[3] != NRFCLAW_NINALINK_MSG_CAPS_REQUEST ||
        wire[6] != 3U)
        return false;

    network_id = get_u16_le(&wire[4]);
    target_node = get_u32_le(&wire[7]);
    request_seq = get_u16_le(&wire[11]);
    reply_to_seq = get_u16_le(&wire[13]);
    page_index = wire[15];

    if (network_id != m_pending_network_id ||
        target_node != NRF_FICR->DEVICEID[0] ||
        reply_to_seq != m_pending_sequence)
        return false;

    if (m_capdisc_last_valid &&
        request_seq == m_capdisc_last_sequence) {
        if (page_index != m_capdisc_last_page)
            return false;
        m_capdisc_duplicate_count++;
    } else {
        m_capdisc_last_valid = true;
        m_capdisc_last_sequence = request_seq;
        m_capdisc_last_page = page_index;
        m_capdisc_served_count++;
    }

    if (!build_capability_response(
            network_id, request_seq, page_index))
        return false;


    ((void)0);

    return true;
}

void nrfclaw_ninalink_link_process(void)
{
    (void)session_try_init();

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
            /*
             * B7.6f2d low-power turnaround fix:
             * once a valid application downlink is accepted, process the
             * APP_RESULT_ARM state in this same main-loop pass. Returning here
             * leaves no wake source before the 60 ms guard is armed, so an
             * autonomous node can sleep for tens of seconds before replying.
             */
            if (accept_cap_set(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
                goto process_app_result_arm;
            }

            if (accept_command(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
                goto process_app_result_arm;
            }

            if (accept_commands_request(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
                goto process_app_result_arm;
            }

            if (accept_capability_request(wire, len)) {
                m_status.state = NRFCLAW_NINALINK_LINK_APP_RESULT_ARM;
                goto process_app_result_arm;
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

process_app_result_arm:
    if (m_status.state == NRFCLAW_NINALINK_LINK_APP_RESULT_ARM) {
        if (!nrfclaw_lora_idle()) {
            return;
        }


        /*
         * B4.12c: allow bridge COMMAND TX -> result RX turnaround.
         * app_timer makes this delay non-blocking and low-power.
         */
        if (!app_result_guard_ready())
            return;


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


bool nrfclaw_ninalink_link_get_session(uint32_t *session_id,
                                       uint16_t *generation)
{
    (void)session_try_init();
    if (session_id)
        *session_id = m_session_id;
    if (generation)
        *generation = m_session_generation;
    return m_session_valid;
}


void nrfclaw_ninalink_link_get_status(nrfclaw_ninalink_link_status_t *out)
{
    if (!out)
        return;

    refresh_totals();
    *out = m_status;
}



void nrfclaw_ninalink_link_get_command_discovery_status(
    nrfclaw_ninalink_command_discovery_node_status_t *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->valid = m_disc_last_valid;
    out->request_seq = m_disc_last_sequence;
    out->start_index = m_disc_last_start;
    out->registry_version = NRFCLAW_NINALINK_COMMAND_REGISTRY_VERSION;
    out->total_count = m_disc_last_total;
    out->count = m_disc_last_count;
    out->served_count = m_disc_served_count;
    out->duplicate_count = m_disc_duplicate_count;
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

void nrfclaw_ninalink_link_get_capability_discovery_status(
    nrfclaw_ninalink_capability_discovery_node_status_t *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->valid = m_capdisc_last_valid;
    out->request_seq = m_capdisc_last_sequence;
    out->page_index = m_capdisc_last_page;
    out->registry_version = NRFCLAW_CAPABILITY_REGISTRY_VERSION;
    out->count = m_capdisc_last_count;
    out->more = m_capdisc_last_more;
    out->served_count = m_capdisc_served_count;
    out->duplicate_count = m_capdisc_duplicate_count;
}
