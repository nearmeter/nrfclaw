#include "nrfclaw_ninalink_lab.h"

#include "app_timer.h"
#include "nrf.h"
#include "sdk_errors.h"
#include "SEGGER_RTT.h"

#include "nrfclaw_capability.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ninalink.h"
#include "nrfclaw_ninalink_msg.h"

#include <string.h>

APP_TIMER_DEF(m_ninalink_lab_timer);

static bool m_initialized;
static volatile bool m_due;
static nrfclaw_ninalink_lab_status_t m_status;

static void lab_timer_handler(void *context)
{
    (void)context;
    if (m_status.active)
        m_due = true;
}

bool nrfclaw_ninalink_lab_init(void)
{
    if (m_initialized)
        return true;

    memset(&m_status, 0, sizeof(m_status));
    m_status.next_sequence = 1U;
    m_status.node_id = NRF_FICR->DEVICEID[0];

    if (app_timer_create(&m_ninalink_lab_timer,
                         APP_TIMER_MODE_REPEATED,
                         lab_timer_handler) != NRF_SUCCESS) {
        m_status.last_result = NRFCLAW_NINALINK_LAB_TIMER_ERROR;
        return false;
    }

    m_initialized = true;
    return true;
}

bool nrfclaw_ninalink_lab_start(uint16_t period_s)
{
    ret_code_t rc;

    if (!m_initialized || period_s < 2U || period_s > 300U)
        return false;

    (void)app_timer_stop(m_ninalink_lab_timer);

    m_due = false;
    m_status.active = false;
    m_status.period_s = period_s;
    m_status.last_result = NRFCLAW_NINALINK_LAB_IDLE;

    rc = app_timer_start(
        m_ninalink_lab_timer,
        APP_TIMER_TICKS((uint32_t)period_s * 1000UL),
        NULL);

    if (rc != NRF_SUCCESS) {
        m_status.last_result = NRFCLAW_NINALINK_LAB_TIMER_ERROR;
        return false;
    }

    m_status.active = true;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.1 LAB: armed node=0x%08lX every=%us first TX after one period\r\n",
        (unsigned long)m_status.node_id,
        (unsigned)period_s);

    return true;
}

void nrfclaw_ninalink_lab_stop(void)
{
    if (!m_initialized)
        return;

    (void)app_timer_stop(m_ninalink_lab_timer);
    m_status.active = false;
    m_due = false;
}

static nrfclaw_ninalink_lab_result_t send_cached_report(void)
{
    nrfclaw_ninalink_value_entry_t entries[2];
    nrfclaw_capability_value_t value;
    nrfclaw_ninalink_frame_t frame;
    uint8_t wire[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t wire_len = 0U;
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

    m_status.last_entry_count = count;

    if (count == 0U) {
        m_status.last_frame_length = 0U;
        return NRFCLAW_NINALINK_LAB_NO_DATA;
    }

    if (nrfclaw_ninalink_build_values(
            &frame,
            NRFCLAW_NINALINK_MSG_CAP_REPORT,
            0x0000U,
            m_status.node_id,
            m_status.next_sequence,
            false,
            entries,
            count) != NRFCLAW_NINALINK_MSG_OK) {
        m_status.last_frame_length = 0U;
        return NRFCLAW_NINALINK_LAB_BUILD_ERROR;
    }

    if (nrfclaw_ninalink_encode(
            &frame,
            wire,
            sizeof(wire),
            &wire_len) != NRFCLAW_NINALINK_OK) {
        m_status.last_frame_length = 0U;
        return NRFCLAW_NINALINK_LAB_BUILD_ERROR;
    }

    m_status.last_frame_length = wire_len;

    if (!nrfclaw_lora_send_async(wire, wire_len))
        return NRFCLAW_NINALINK_LAB_RADIO_BUSY;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.1 LAB: TX node=0x%08lX seq=%u entries=%u len=%u\r\n",
        (unsigned long)m_status.node_id,
        (unsigned)m_status.next_sequence,
        (unsigned)count,
        (unsigned)wire_len);

    m_status.next_sequence++;
    return NRFCLAW_NINALINK_LAB_SENT;
}


static uint16_t lab_crc16_ccitt_false(const uint8_t *data, uint8_t len)
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

static void lab_put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void lab_put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool nrfclaw_ninalink_lab_send_max_test(void)
{
    static const uint16_t cap_ids[6] = {
        0x0102U, /* illuminance */
        0x0103U, /* pressure */
        0x0105U, /* tvoc */
        0x0302U, /* counter */
        0x0304U, /* pulse_frequency */
        0x0503U  /* energy */
    };

    uint8_t wire[64];
    uint8_t off;
    uint8_t i;
    uint16_t crc;
    uint16_t seq;

    memset(wire, 0, sizeof(wire));

    seq = m_status.next_sequence;

    wire[0] = 0x4EU; /* 'N' */
    wire[1] = 0x01U; /* NinaLink v1 */
    wire[2] = 0x00U; /* flags */
    wire[3] = 0x10U; /* CAP_REPORT */
    lab_put_u16_le(&wire[4], 0x0000U);
    wire[6] = 49U;
    lab_put_u32_le(&wire[7], m_status.node_id);
    lab_put_u16_le(&wire[11], seq);

    wire[13] = 6U;
    off = 14U;

    for (i = 0U; i < 6U; i++) {
        lab_put_u16_le(&wire[off], cap_ids[i]);
        wire[off + 2U] = 0U;
        wire[off + 3U] = 0x06U; /* U32 */
        lab_put_u32_le(&wire[off + 4U], (uint32_t)(i + 1U) * 1000UL);
        off = (uint8_t)(off + 8U);
    }

    if (off != 62U) {
        m_status.last_result = NRFCLAW_NINALINK_LAB_BUILD_ERROR;
        return false;
    }

    crc = lab_crc16_ccitt_false(wire, 62U);
    lab_put_u16_le(&wire[62], crc);

    m_status.last_entry_count = 6U;
    m_status.last_frame_length = 64U;

    if (!nrfclaw_lora_send_async(wire, sizeof(wire))) {
        m_status.last_result = NRFCLAW_NINALINK_LAB_RADIO_BUSY;
        return false;
    }

    m_status.last_result = NRFCLAW_NINALINK_LAB_SENT;
    m_status.next_sequence++;

    SEGGER_RTT_printf(
        0,
        "NINALINK B4.2 MAX: TX node=0x%08lX seq=%u entries=6 len=64 crc=0x%04X\r\n",
        (unsigned long)m_status.node_id,
        (unsigned)seq,
        (unsigned)crc);

    return true;
}

void nrfclaw_ninalink_lab_process(void)
{
    nrfclaw_ninalink_lab_result_t result;

    if (!m_initialized || !m_status.active || !m_due)
        return;

    m_due = false;

    result = send_cached_report();
    m_status.last_result = (uint8_t)result;
}

void nrfclaw_ninalink_lab_get_status(nrfclaw_ninalink_lab_status_t *out)
{
    if (out)
        *out = m_status;
}
