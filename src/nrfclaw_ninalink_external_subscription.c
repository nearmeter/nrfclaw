#include "nrfclaw_ninalink_external_subscription.h"

#include "nrfclaw_ble_app.h"
#include "nrfclaw_ninalink_external.h"
#include "nrfclaw_ninalink_state_cache.h"

#include <stddef.h>
#include <string.h>

static uint8_t m_mask;
static uint8_t m_pending_flags;
static uint32_t m_last_state_revision;
static uint32_t m_last_event_revision;
static uint32_t m_pending_state_revision;
static uint32_t m_pending_event_revision;
static uint32_t m_pending_newest_event_id;
static uint32_t m_change_revision;
static uint32_t m_wait_tx_generation;
static bool m_waiting_for_tx_progress;
static uint16_t m_notifications_sent;
static uint16_t m_coalesced;
static uint16_t m_busy_retries;
static uint16_t m_disconnect_resets;
static uint16_t m_send_errors;

static uint16_t sat_inc16(uint16_t value)
{
    return value == 0xFFFFU ? value : (uint16_t)(value + 1U);
}

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0]=(uint8_t)value;
    p[1]=(uint8_t)(value>>8);
    p[2]=(uint8_t)(value>>16);
    p[3]=(uint8_t)(value>>24);
}

void nrfclaw_ninalink_external_subscription_set(uint8_t mask)
{
    uint32_t state_revision = 0U;
    uint32_t event_revision = 0U;
    uint32_t change_revision = 0U;

    mask &= NRFCLAW_NINALINK_CHANGE_ALL;
    nrfclaw_ninalink_state_cache_get_revisions(
        &state_revision,&event_revision,&change_revision);

    m_mask = mask;
    m_pending_flags = 0U;
    m_last_state_revision = state_revision;
    m_last_event_revision = event_revision;
    m_pending_state_revision = state_revision;
    m_pending_event_revision = event_revision;
    m_change_revision = change_revision;
    m_waiting_for_tx_progress = false;
    m_wait_tx_generation = nrfclaw_ble_app_tx_generation();

    if (mask != 0U) {
        nrfclaw_ninalink_external_status_t ext;
        nrfclaw_ninalink_external_get_status(&ext);
        m_pending_newest_event_id = ext.newest_event_id;
    } else {
        m_pending_newest_event_id = 0U;
    }
}

void nrfclaw_ninalink_external_subscription_reset(void)
{
    if (m_mask != 0U)
        m_disconnect_resets = sat_inc16(m_disconnect_resets);
    m_mask = 0U;
    m_pending_flags = 0U;
    m_waiting_for_tx_progress = false;
}

void nrfclaw_ninalink_external_subscription_get_status(
    nrfclaw_ninalink_change_status_t *out)
{
    nrfclaw_ninalink_external_status_t ext;

    if (!out)
        return;

    memset(out,0,sizeof(*out));
    nrfclaw_ninalink_state_cache_get_revisions(
        &out->state_revision,&out->event_revision,NULL);
    nrfclaw_ninalink_external_get_status(&ext);
    out->schema_version = NRFCLAW_NINALINK_CHANGE_SCHEMA_VERSION;
    out->mask = m_mask;
    out->pending_flags = m_pending_flags;
    out->newest_event_id = ext.newest_event_id;
}

void nrfclaw_ninalink_external_subscription_get_stats(
    nrfclaw_ninalink_change_stats_t *out)
{
    uint32_t change_revision = 0U;
    if (!out)
        return;
    nrfclaw_ninalink_state_cache_get_revisions(
        NULL,NULL,&change_revision);
    memset(out,0,sizeof(*out));
    out->change_revision = change_revision;
    out->notifications_sent = m_notifications_sent;
    out->coalesced = m_coalesced;
    out->busy_retries = m_busy_retries;
    out->disconnect_resets = m_disconnect_resets;
    out->send_errors = m_send_errors;
}

void nrfclaw_ninalink_external_subscription_process(void)
{
    uint32_t state_revision = 0U;
    uint32_t event_revision = 0U;
    uint32_t change_revision = 0U;
    uint8_t flags = 0U;
    uint8_t frame[NRFCLAW_NINALINK_CHANGE_FRAME_SIZE];
    nrfclaw_ninalink_external_status_t ext;
    nrfclaw_ble_app_status_t status;
    uint32_t tx_generation;

    if (m_mask == 0U)
        return;

    if (!nrfclaw_ble_app_connected()) {
        nrfclaw_ninalink_external_subscription_reset();
        return;
    }

    nrfclaw_ninalink_state_cache_get_revisions(
        &state_revision,&event_revision,&change_revision);

    if ((m_mask & NRFCLAW_NINALINK_CHANGE_STATE) != 0U &&
        state_revision != m_last_state_revision)
        flags |= NRFCLAW_NINALINK_CHANGE_STATE;
    if ((m_mask & NRFCLAW_NINALINK_CHANGE_EVENT) != 0U &&
        event_revision != m_last_event_revision)
        flags |= NRFCLAW_NINALINK_CHANGE_EVENT;

    if (flags != 0U) {
        if (m_pending_flags != 0U &&
            (state_revision != m_pending_state_revision ||
             event_revision != m_pending_event_revision))
            m_coalesced = sat_inc16(m_coalesced);

        m_pending_flags |= flags;
        m_pending_state_revision = state_revision;
        m_pending_event_revision = event_revision;
        m_change_revision = change_revision;
        nrfclaw_ninalink_external_get_status(&ext);
        m_pending_newest_event_id = ext.newest_event_id;
    }

    if (m_pending_flags == 0U)
        return;

    if (m_waiting_for_tx_progress) {
        tx_generation = nrfclaw_ble_app_tx_generation();
        if (tx_generation == m_wait_tx_generation)
            return;
        m_waiting_for_tx_progress = false;
    }

    memset(frame,0,sizeof(frame));
    frame[0]=NRFCLAW_NINALINK_CHANGE_MAGIC;
    frame[1]=NRFCLAW_NINALINK_CHANGE_SCHEMA_VERSION;
    frame[2]=m_pending_flags;
    frame[3]=m_mask;
    put_u32_le(&frame[4],m_pending_state_revision);
    put_u32_le(&frame[8],m_pending_event_revision);
    put_u32_le(&frame[12],m_pending_newest_event_id);
    put_u32_le(&frame[16],m_change_revision);

    status = nrfclaw_ble_app_send_raw(frame,sizeof(frame));
    if (status == NRFCLAW_BLE_APP_OK) {
        m_notifications_sent = sat_inc16(m_notifications_sent);
        m_last_state_revision = m_pending_state_revision;
        m_last_event_revision = m_pending_event_revision;
        m_pending_flags = 0U;
        m_waiting_for_tx_progress = false;
        return;
    }

    if (status == NRFCLAW_BLE_APP_RESOURCES ||
        status == NRFCLAW_BLE_APP_BUSY) {
        m_busy_retries = sat_inc16(m_busy_retries);
        m_waiting_for_tx_progress = true;
        m_wait_tx_generation = nrfclaw_ble_app_tx_generation();
        return;
    }

    if (status == NRFCLAW_BLE_APP_NOT_CONNECTED) {
        nrfclaw_ninalink_external_subscription_reset();
        return;
    }

    m_send_errors = sat_inc16(m_send_errors);
}
