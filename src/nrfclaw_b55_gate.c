#include "nrfclaw_b55_gate.h"

#include "app_timer.h"
#include "nrfclaw_ninalink_link.h"

#include <string.h>

#define B55_GATE_MIN_STATE_DELAY_S  5U
#define B55_GATE_MAX_STATE_DELAY_S 60U
#define B55_GATE_MIN_EVENT_GAP_S    2U
#define B55_GATE_MAX_EVENT_GAP_S   60U

#define B55_GATE_ACK_WINDOW_MS     600U
#define B55_GATE_ATTEMPTS            3U
#define B55_GATE_BACKOFF_MS        200U
#define B55_GATE_EVENT_TAP           1U

/* Deterministic STATE stimulus for the hardware gate.
 * seq=0 keeps the following synthetic event at production next_sequence=1,
 * so the observed stream remains naturally monotonic within the session.
 */
#define B55_GATE_STATE_TEMP_CENTI  2181
#define B55_GATE_STATE_SEQUENCE       0U

APP_TIMER_DEF(m_b55_gate_timer);

static bool m_timer_initialized;
static volatile bool m_due;
static uint8_t m_event_gap_s;
static nrfclaw_b55_gate_status_t m_status;

static void timer_handler(void *context)
{
    (void)context;
    m_due = true;
}

static bool ensure_timer(void)
{
    if (m_timer_initialized)
        return true;

    if (app_timer_create(
            &m_b55_gate_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            timer_handler) != NRF_SUCCESS)
        return false;

    m_timer_initialized = true;
    return true;
}

static bool start_delay(uint8_t seconds)
{
    m_due = false;
    return app_timer_start(
               m_b55_gate_timer,
               APP_TIMER_TICKS((uint32_t)seconds * 1000UL),
               NULL) == NRF_SUCCESS;
}

static void set_error(uint8_t error)
{
    m_status.stage = NRFCLAW_B55_GATE_ERROR;
    m_status.error = error;
    m_status.armed = false;
    m_due = false;
}

bool nrfclaw_b55_gate_arm(uint8_t state_delay_s, uint8_t event_gap_s)
{
    nrfclaw_ninalink_link_status_t link;

    if (state_delay_s < B55_GATE_MIN_STATE_DELAY_S ||
        state_delay_s > B55_GATE_MAX_STATE_DELAY_S ||
        event_gap_s < B55_GATE_MIN_EVENT_GAP_S ||
        event_gap_s > B55_GATE_MAX_EVENT_GAP_S)
        return false;

    nrfclaw_ninalink_link_get_status(&link);
    if (link.state != NRFCLAW_NINALINK_LINK_IDLE &&
        link.state != NRFCLAW_NINALINK_LINK_DONE)
        return false;

    if (!ensure_timer()) {
        memset(&m_status, 0, sizeof(m_status));
        set_error(NRFCLAW_B55_GATE_ERR_TIMER_CREATE);
        return false;
    }

    (void)app_timer_stop(m_b55_gate_timer);

    memset(&m_status, 0, sizeof(m_status));
    m_status.stage = NRFCLAW_B55_GATE_WAIT_STATE;
    m_status.armed = true;
    m_event_gap_s = event_gap_s;

    if (!start_delay(state_delay_s)) {
        set_error(NRFCLAW_B55_GATE_ERR_TIMER_START);
        return false;
    }

    return true;
}

void nrfclaw_b55_gate_process(void)
{
    nrfclaw_ninalink_link_status_t link;

    if (m_status.stage == NRFCLAW_B55_GATE_WAIT_STATE) {
        if (!m_due)
            return;

        m_due = false;

        /*
         * Use the already validated B5.3c deterministic CAP_REPORT emitter.
         * Do not use start_reliable() here: its normal report builder depends
         * on battery/temperature having first been primed into the capability
         * runtime cache by the host CLI.
         *
         * The no-dual-BLE gate must be autonomous after NUS disconnect, so
         * its STATE stimulus must not depend on a preceding host sensor read.
         */
        if (!nrfclaw_ninalink_link_start_state_test(
                B55_GATE_STATE_TEMP_CENTI,
                B55_GATE_STATE_SEQUENCE,
                B55_GATE_ACK_WINDOW_MS,
                B55_GATE_ATTEMPTS,
                B55_GATE_BACKOFF_MS)) {
            set_error(NRFCLAW_B55_GATE_ERR_STATE_START);
            return;
        }

        m_status.stage = NRFCLAW_B55_GATE_STATE_RUNNING;
        return;
    }

    if (m_status.stage == NRFCLAW_B55_GATE_STATE_RUNNING) {
        nrfclaw_ninalink_link_get_status(&link);
        if (link.state != NRFCLAW_NINALINK_LINK_DONE)
            return;

        m_status.state_result = link.result;
        if (link.result != NRFCLAW_NINALINK_LINK_RESULT_ACKED) {
            set_error(NRFCLAW_B55_GATE_ERR_STATE_RESULT);
            return;
        }

        m_status.state_sent = true;
        m_status.stage = NRFCLAW_B55_GATE_WAIT_EVENT;

        if (!start_delay(m_event_gap_s)) {
            set_error(NRFCLAW_B55_GATE_ERR_TIMER_START);
            return;
        }
        return;
    }

    if (m_status.stage == NRFCLAW_B55_GATE_WAIT_EVENT) {
        if (!m_due)
            return;

        m_due = false;

        if (!nrfclaw_ninalink_link_start_event_test(
                B55_GATE_EVENT_TAP,
                B55_GATE_ACK_WINDOW_MS,
                B55_GATE_ATTEMPTS,
                B55_GATE_BACKOFF_MS)) {
            set_error(NRFCLAW_B55_GATE_ERR_EVENT_START);
            return;
        }

        m_status.stage = NRFCLAW_B55_GATE_EVENT_RUNNING;
        return;
    }

    if (m_status.stage == NRFCLAW_B55_GATE_EVENT_RUNNING) {
        nrfclaw_ninalink_link_get_status(&link);
        if (link.state != NRFCLAW_NINALINK_LINK_DONE)
            return;

        m_status.event_result = link.result;
        if (link.result != NRFCLAW_NINALINK_LINK_RESULT_ACKED) {
            set_error(NRFCLAW_B55_GATE_ERR_EVENT_RESULT);
            return;
        }

        m_status.event_sent = true;
        m_status.stage = NRFCLAW_B55_GATE_DONE;
        m_status.armed = false;
    }
}

void nrfclaw_b55_gate_get_status(nrfclaw_b55_gate_status_t *out)
{
    if (out)
        *out = m_status;
}
