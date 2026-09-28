#include "nrfclaw_ha_ninalink_node.h"

#include "app_timer.h"
#include "nrf_error.h"

#include "nrfclaw_battery.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_ninalink_link.h"

#include <string.h>

typedef enum {
    HA_NODE_IDLE = 0,
    HA_NODE_WAIT_SENSORS = 1,
    HA_NODE_WAIT_LINK = 2
} ha_node_stage_t;

APP_TIMER_DEF(m_ha_node_timer);

static bool m_timer_initialized;
static volatile bool m_tick_due;
static bool m_wait_battery;
static bool m_wait_temperature;
static uint8_t m_cycle;
static uint16_t m_period_s = NRFCLAW_HA_NODE_DEFAULT_PERIOD_S;
static nrfclaw_ha_ninalink_node_status_t m_status;

static void timer_handler(void *ctx)
{
    (void)ctx;
    m_tick_due = true;
}

static bool ensure_timer(void)
{
    if (m_timer_initialized)
        return true;
    if (app_timer_create(&m_ha_node_timer,
                         APP_TIMER_MODE_REPEATED,
                         timer_handler) != NRF_SUCCESS)
        return false;
    m_timer_initialized = true;
    return true;
}

bool nrfclaw_ha_ninalink_node_start(uint16_t period_s)
{
    if (period_s < NRFCLAW_HA_NODE_MIN_PERIOD_S ||
        period_s > NRFCLAW_HA_NODE_MAX_PERIOD_S)
        return false;
    if (!ensure_timer())
        return false;

    m_period_s = period_s;

    (void)app_timer_stop(m_ha_node_timer);
    memset(&m_status, 0, sizeof(m_status));
    m_status.enabled = true;
    m_status.stage = HA_NODE_IDLE;
    m_status.period_s = m_period_s;
    m_cycle = 0U;
    m_wait_battery = false;
    m_wait_temperature = false;

    /* Immediate first contact gives bridge/HA discovery without waiting a full
     * reporting period. Subsequent contacts retain the configured fixed phase. */
    m_tick_due = true;

    if (app_timer_start(
            m_ha_node_timer,
            APP_TIMER_TICKS((uint32_t)m_period_s * 1000UL),
            NULL) != NRF_SUCCESS) {
        m_status.enabled = false;
        return false;
    }
    return true;
}

void nrfclaw_ha_ninalink_node_stop(void)
{
    if (m_timer_initialized)
        (void)app_timer_stop(m_ha_node_timer);
    m_status.enabled = false;
    m_status.stage = HA_NODE_IDLE;
    m_tick_due = false;
    m_wait_battery = false;
    m_wait_temperature = false;
}

void nrfclaw_ha_ninalink_node_on_event(const nrfclaw_event_t *evt)
{
    if (!evt || !m_status.enabled ||
        m_status.stage != HA_NODE_WAIT_SENSORS)
        return;

    if (evt->type == NRFCLAW_EVT_BATTERY_DONE && m_wait_battery)
        m_wait_battery = false;
    else if (evt->type == NRFCLAW_EVT_TEMPERATURE_DONE && m_wait_temperature)
        m_wait_temperature = false;
}

static bool start_report(void)
{
    if (!nrfclaw_ninalink_link_start_reliable(
            NRFCLAW_HA_NODE_ACK_WINDOW_MS,
            NRFCLAW_HA_NODE_ATTEMPTS,
            NRFCLAW_HA_NODE_BACKOFF_MS))
        return false;

    if (m_status.reports_started != 0xFFFFFFFFUL)
        m_status.reports_started++;
    m_status.stage = HA_NODE_WAIT_LINK;
    return true;
}

static void start_cycle(void)
{
    bool started = false;

    if (!m_status.enabled || m_status.stage != HA_NODE_IDLE || !m_tick_due)
        return;

    m_tick_due = false;
    m_cycle++;
    m_wait_battery = false;
    m_wait_temperature = false;

    /* Battery is refreshed on first contact and every fifth report. Cached
     * values are still included by the existing NinaLink report builder on
     * intermediate contacts. */
    if (m_cycle == 1U || (m_cycle % 5U) == 0U) {
        if (!nrfclaw_battery_busy() && nrfclaw_battery_start()) {
            m_wait_battery = true;
            started = true;
        } else if (nrfclaw_battery_busy()) {
            m_status.sensor_errors++;
        }
    }

    if (!nrfclaw_temperature_busy() && nrfclaw_temperature_start()) {
        m_wait_temperature = true;
        started = true;
    } else if (nrfclaw_temperature_busy()) {
        m_status.sensor_errors++;
    }

    if (started) {
        m_status.stage = HA_NODE_WAIT_SENSORS;
        return;
    }

    /* If no acquisition was needed, current/cached capabilities can be sent
     * immediately. A transient radio busy condition is retried by process(). */
    if (!start_report())
        m_status.stage = HA_NODE_WAIT_SENSORS;
}

void nrfclaw_ha_ninalink_node_process(void)
{
    nrfclaw_ninalink_link_status_t link;

    if (!m_status.enabled)
        return;

    if (m_status.stage == HA_NODE_IDLE) {
        start_cycle();
        return;
    }

    if (m_status.stage == HA_NODE_WAIT_SENSORS) {
        if (m_wait_battery || m_wait_temperature)
            return;
        (void)start_report();
        return;
    }

    if (m_status.stage != HA_NODE_WAIT_LINK)
        return;

    nrfclaw_ninalink_link_get_status(&link);
    if (link.state != NRFCLAW_NINALINK_LINK_DONE)
        return;

    m_status.last_link_result = (uint8_t)link.result;
    if (link.result == NRFCLAW_NINALINK_LINK_RESULT_ACKED)
        m_status.acked++;
    else if (link.result == NRFCLAW_NINALINK_LINK_RESULT_TIMEOUT)
        m_status.timed_out++;

    m_status.reports_completed++;
    m_status.stage = HA_NODE_IDLE;
    start_cycle();
}

void nrfclaw_ha_ninalink_node_get_status(
    nrfclaw_ha_ninalink_node_status_t *out)
{
    if (out)
        *out = m_status;
}
