#include "nrfclaw_ninalink_telemetry.h"

#include "app_timer.h"
#include "nrf_error.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_capability.h"

#include <string.h>

#define TELEM_PERIOD_MIN_S       5U
#define TELEM_PERIOD_MAX_S       300U
#define TELEM_ACK_MIN_MS         100U
#define TELEM_ACK_MAX_MS         4000U
#define TELEM_ATTEMPTS_MAX       5U
#define TELEM_BACKOFF_MAX_MS     4000U

APP_TIMER_DEF(m_telem_timer);

static bool m_timer_initialized;
static volatile bool m_tick_due;
static volatile uint32_t m_ticks;
static bool m_temp_ready;
static int32_t m_temp_mC;
static bool m_synthetic_source;
static int32_t m_synthetic_temperature_mC;

static nrfclaw_ninalink_telemetry_status_t m_status;

static void telemetry_timer_handler(void *ctx)
{
    (void)ctx;

    /*
     * B4.12a timing fix:
     * m_tick_due is a one-deep pending latch. Never destroy a valid
     * periodic edge merely because the previous reliable transaction has
     * not yet had its DONE state consumed by the main loop.
     */
    if (m_tick_due) {
        if (m_status.overruns != 0xFFFFFFFFUL)
            m_status.overruns++;
    } else {
        m_tick_due = true;
    }

    if (m_ticks != 0xFFFFFFFFUL)
        m_ticks++;
}

static bool ensure_timer(void)
{
    uint32_t err;

    if (m_timer_initialized)
        return true;

    err = app_timer_create(
        &m_telem_timer,
        APP_TIMER_MODE_REPEATED,
        telemetry_timer_handler);

    if (err != NRF_SUCCESS)
        return false;

    m_timer_initialized = true;
    return true;
}

void nrfclaw_ninalink_telemetry_init(void)
{
    memset(&m_status, 0, sizeof(m_status));
    m_status.stage = NRFCLAW_NINALINK_TELEM_IDLE;
    m_tick_due = false;
    m_temp_ready = false;
    m_temp_mC = 0;
}

static bool telemetry_start_common(
    uint16_t period_s,
    uint16_t ack_window_ms,
    uint8_t max_attempts,
    uint16_t base_backoff_ms,
    bool synthetic_source,
    int32_t synthetic_temperature_mC)
{
    uint32_t err;

    if (period_s < TELEM_PERIOD_MIN_S ||
        period_s > TELEM_PERIOD_MAX_S)
        return false;

    if (ack_window_ms < TELEM_ACK_MIN_MS ||
        ack_window_ms > TELEM_ACK_MAX_MS)
        return false;

    if (max_attempts == 0U ||
        max_attempts > TELEM_ATTEMPTS_MAX)
        return false;

    if (max_attempts > 1U &&
        (base_backoff_ms == 0U ||
         base_backoff_ms > TELEM_BACKOFF_MAX_MS))
        return false;

    if (!ensure_timer())
        return false;

    /*
     * B4.12 power invariant:
     *
     * A periodic NinaLink telemetry endpoint is a LoRa node, not a local
     * Application/NDP BLE endpoint. HA reaches this node through the bridge.
     *
     * suspend() stops advertising and cancels deferred advertising restart.
     * Force role OFF afterwards so Application/NDP cannot re-arm itself while
     * periodic telemetry owns the node.
     *
     * P0.21 remains available for programming/NUS maintenance.
     */
    nrfclaw_ble_app_suspend();
    if (nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_OFF) !=
        NRFCLAW_BLE_APP_OK)
        return false;

    (void)app_timer_stop(m_telem_timer);

    m_synthetic_source = synthetic_source;
    m_synthetic_temperature_mC = synthetic_temperature_mC;

    if (m_synthetic_source)
        nrfclaw_capability_temperature_override_set(
            m_synthetic_temperature_mC);
    else
        nrfclaw_capability_temperature_override_clear();
    if (m_synthetic_source)
        nrfclaw_temperature_idle_lowpower();

    memset(&m_status, 0, sizeof(m_status));
    m_status.enabled = true;
    m_status.stage = NRFCLAW_NINALINK_TELEM_IDLE;
    m_status.period_s = period_s;
    m_status.ack_window_ms = ack_window_ms;
    m_status.max_attempts = max_attempts;
    m_status.base_backoff_ms =
        max_attempts > 1U ? base_backoff_ms : 0U;

    m_tick_due = false;
    m_ticks = 0U;
    m_temp_ready = false;

    /*
     * Fixed phase: first acquisition starts after one complete period.
     * Transaction duration does not move subsequent timer edges.
     */
    err = app_timer_start(
        m_telem_timer,
        APP_TIMER_TICKS((uint32_t)period_s * 1000UL),
        NULL);

    if (err != NRF_SUCCESS) {
        m_status.enabled = false;
        return false;
    }

    ((void)0);

    return true;
}

bool nrfclaw_ninalink_telemetry_start(
    uint16_t period_s, uint16_t ack_window_ms, uint8_t max_attempts, uint16_t base_backoff_ms)
{
    return telemetry_start_common(period_s, ack_window_ms, max_attempts, base_backoff_ms, false, 0);
}

bool nrfclaw_ninalink_telemetry_start_synthetic(
    uint16_t period_s, uint16_t ack_window_ms, uint8_t max_attempts, uint16_t base_backoff_ms, int32_t temperature_mC)
{
    if (temperature_mC < -55000L || temperature_mC > 125000L) return false;
    return telemetry_start_common(period_s, ack_window_ms, max_attempts, base_backoff_ms, true, temperature_mC);
}

void nrfclaw_ninalink_telemetry_stop(void)
{
    nrfclaw_capability_temperature_override_clear();
    if (m_timer_initialized)
        (void)app_timer_stop(m_telem_timer);

    if (m_status.stage == NRFCLAW_NINALINK_TELEM_WAIT_TEMP)
        nrfclaw_temperature_idle_lowpower();

    m_status.enabled = false;
    m_status.stage = NRFCLAW_NINALINK_TELEM_IDLE;
    m_tick_due = false;
    m_temp_ready = false;
}

void nrfclaw_ninalink_telemetry_on_event(const nrfclaw_event_t *evt)
{
    if (!evt || !m_status.enabled)
        return;

    if (evt->type == NRFCLAW_EVT_TEMPERATURE_DONE &&
        !m_synthetic_source &&
        m_status.stage == NRFCLAW_NINALINK_TELEM_WAIT_TEMP) {
        m_temp_mC = (int32_t)evt->arg0;
        m_temp_ready = true;
    }
}


static void telemetry_start_pending_cycle(void)
{
    if (!m_status.enabled ||
        m_status.stage != NRFCLAW_NINALINK_TELEM_IDLE ||
        !m_tick_due)
        return;

    m_tick_due = false;

    if (m_synthetic_source) {
        /*
         * B4.12a power-test:
         * Synthetic temperature has no asynchronous sensor completion event.
         * Therefore the reliable NinaLink transaction MUST start on this same
         * timer wake. Routing synthetic data through WAIT_TEMP makes each
         * transmission require a second timer interrupt and doubles the
         * observed RF period.
         */
        m_temp_mC = m_synthetic_temperature_mC;
        m_temp_ready = false;

        if (!nrfclaw_ninalink_link_start_temperature_mC(
                m_temp_mC,
                m_status.ack_window_ms,
                m_status.max_attempts,
                m_status.base_backoff_ms)) {
            m_tick_due = true;
            return;
        }

        m_status.last_temperature_mC = m_temp_mC;
        if (m_status.cycles_started != 0xFFFFFFFFUL)
            m_status.cycles_started++;

        m_status.stage = NRFCLAW_NINALINK_TELEM_WAIT_LINK;
        return;
    }

    if (!nrfclaw_temperature_start()) {
        if (m_status.sensor_errors != 0xFFFFFFFFUL)
            m_status.sensor_errors++;
        return;
    }

    m_temp_ready = false;
    m_status.stage = NRFCLAW_NINALINK_TELEM_WAIT_TEMP;
}

void nrfclaw_ninalink_telemetry_process(void)
{
    nrfclaw_ninalink_link_status_t ls;

    if (!m_status.enabled)
        return;

    m_status.ticks = m_ticks;

        if (m_status.stage == NRFCLAW_NINALINK_TELEM_IDLE) {
        telemetry_start_pending_cycle();
        return;
    }

    /*
     * B4.12a timing fix:
     * Preserve a periodic edge while the current reliable transaction is
     * finishing. Once stage returns to IDLE, that pending edge is consumed
     * as the next scheduled cycle.
     */

    if (m_status.stage == NRFCLAW_NINALINK_TELEM_WAIT_TEMP) {
        if (!m_temp_ready)
            return;

        /*
         * A transiently busy radio does not discard the fresh sample.
         * Keep retrying from the main loop until the production link path
         * accepts the logical transaction.
         */
        if (!nrfclaw_ninalink_link_start_temperature_mC(
                m_temp_mC,
                m_status.ack_window_ms,
                m_status.max_attempts,
                m_status.base_backoff_ms))
            return;

        m_temp_ready = false;
        m_status.last_temperature_mC = m_temp_mC;
        if (m_status.cycles_started != 0xFFFFFFFFUL)
            m_status.cycles_started++;
        m_status.stage = NRFCLAW_NINALINK_TELEM_WAIT_LINK;
        return;
    }

    if (m_status.stage != NRFCLAW_NINALINK_TELEM_WAIT_LINK)
        return;

    nrfclaw_ninalink_link_get_status(&ls);
    if (ls.state != NRFCLAW_NINALINK_LINK_DONE)
        return;

    m_status.last_link_result = (uint8_t)ls.result;

    if (ls.result == NRFCLAW_NINALINK_LINK_RESULT_ACKED) {
        if (m_status.acked != 0xFFFFFFFFUL)
            m_status.acked++;
    } else if (ls.result == NRFCLAW_NINALINK_LINK_RESULT_TIMEOUT) {
        /*
         * Bridge OFF is a normal bounded power-gate outcome.
         * Do not disable telemetry when the gateway disappears.
         */
        if (m_status.timed_out != 0xFFFFFFFFUL)
            m_status.timed_out++;
    } else {
        if (m_status.link_errors != 0xFFFFFFFFUL)
            m_status.link_errors++;
    }

    if (m_status.cycles_completed != 0xFFFFFFFFUL)
        m_status.cycles_completed++;

    m_status.stage = NRFCLAW_NINALINK_TELEM_IDLE;

    /*
     * A pending timer edge is only a RAM flag and cannot wake the CPU after
     * power management sleeps. Consume it now when the link reaches DONE.
     */
    telemetry_start_pending_cycle();
}

void nrfclaw_ninalink_telemetry_get_status(
    nrfclaw_ninalink_telemetry_status_t *out)
{
    if (out) {
        *out = m_status;
        out->ticks = m_ticks;
    }
}
