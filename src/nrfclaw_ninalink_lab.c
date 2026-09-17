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
