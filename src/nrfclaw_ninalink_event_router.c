#include "nrfclaw_ninalink_event_router.h"

#include "nrfclaw_capability.h"
#include "nrfclaw_ha_role.h"
#include "nrfclaw_native.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_ninalink_link.h"

#include <stdbool.h>
#include <string.h>

#ifndef NRFCLAW_EXPERIMENTAL_VIB_AUTO
#define NRFCLAW_EXPERIMENTAL_VIB_AUTO 0
#endif

#define EVENT_ACK_WINDOW_MS 600U
#define EVENT_MAX_ATTEMPTS  3U
#define EVENT_BACKOFF_MS    200U

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
    uint8_t value_type;
    uint32_t raw_value;
} queued_event_t;

static queued_event_t m_queue[NRFCLAW_NINALINK_EVENT_ROUTER_DEPTH];
static uint8_t m_head;
static uint8_t m_tail;
static uint8_t m_count;

static uint16_t m_enqueued;
static uint16_t m_sent;
static uint16_t m_dropped;
static uint16_t m_ignored;
static uint16_t m_last_capability_id;

static void inc16(uint16_t *value)
{
    if (value && *value != 0xFFFFU)
        (*value)++;
}

static bool map_event(
    const nrfclaw_event_t *event,
    queued_event_t *out)
{
    if (!event || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->channel = 0U;

    /*
     * VIB_AUTO uses legacy extension event IDs 64..68. Those IDs
     * are intentionally outside the core enum enumerators, but are
     * transported through the same event bus. Switch on the integer
     * representation so -Wswitch accepts those extension IDs.
     */
    switch ((uint32_t)event->type) {
        case NRFCLAW_EVT_ACCEL_MOTION:
            /*
             * Direct parity: while VIB_AUTO owns the accelerometer, MOTION is
             * an internal machine-wake primitive and must not leak as user
             * Motion. VIB_AUTO semantic events are routed separately in g3.
             */
            if (nrfclaw_native_capability_active(NRFCLAW_CAP_VIB_AUTO))
                return false;
            out->capability_id = NRFCLAW_SEMCAP_MOTION;
            out->value_type = NRFCLAW_CAP_VALUE_BOOL;
            out->raw_value = 1U;
            return true;

#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
        case NRFCLAW_EVT_VIB_AUTO_WARNING:
            out->capability_id = NRFCLAW_SEMCAP_VIBRATION_EVENT;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = NRFCLAW_VIB_EVENT_WARNING;
            return true;

        case NRFCLAW_EVT_VIB_AUTO_ALARM:
            out->capability_id = NRFCLAW_SEMCAP_VIBRATION_EVENT;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = NRFCLAW_VIB_EVENT_ALARM;
            return true;

        case NRFCLAW_EVT_VIB_AUTO_MACHINE_ON:
            out->capability_id = NRFCLAW_SEMCAP_VIBRATION_EVENT;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = NRFCLAW_VIB_EVENT_MACHINE_ON;
            return true;

        case NRFCLAW_EVT_VIB_AUTO_MACHINE_OFF:
            out->capability_id = NRFCLAW_SEMCAP_VIBRATION_EVENT;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = NRFCLAW_VIB_EVENT_MACHINE_OFF;
            return true;

        case NRFCLAW_EVT_VIB_AUTO_LEARN_COMPLETE:
            out->capability_id = NRFCLAW_SEMCAP_VIBRATION_EVENT;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = NRFCLAW_VIB_EVENT_LEARN_COMPLETE;
            return true;
#endif

        case NRFCLAW_EVT_ACCEL_TAP:
            out->capability_id = NRFCLAW_SEMCAP_TAP;
            out->value_type = NRFCLAW_CAP_VALUE_ENUM8;
            out->raw_value = 2U;
            return true;

        case NRFCLAW_EVT_ACCEL_FALL:
            out->capability_id = NRFCLAW_SEMCAP_FALL;
            out->value_type = NRFCLAW_CAP_VALUE_BOOL;
            out->raw_value = 1U;
            return true;

        case NRFCLAW_EVT_ACCEL_WALK:
            out->capability_id = NRFCLAW_SEMCAP_WALK;
            out->value_type = NRFCLAW_CAP_VALUE_BOOL;
            out->raw_value = 1U;
            return true;

        default:
            return false;
    }
}

static bool queue_push(const queued_event_t *event)
{
    if (!event)
        return false;

    if (m_count >= NRFCLAW_NINALINK_EVENT_ROUTER_DEPTH) {
        inc16(&m_dropped);
        return false;
    }

    m_queue[m_tail] = *event;
    m_tail = (uint8_t)(
        (m_tail + 1U) % NRFCLAW_NINALINK_EVENT_ROUTER_DEPTH);
    m_count++;
    inc16(&m_enqueued);
    m_last_capability_id = event->capability_id;
    return true;
}

static void queue_pop(void)
{
    if (m_count == 0U)
        return;

    m_head = (uint8_t)(
        (m_head + 1U) % NRFCLAW_NINALINK_EVENT_ROUTER_DEPTH);
    m_count--;
}

void nrfclaw_ninalink_event_router_on_event(
    const nrfclaw_event_t *event)
{
    queued_event_t mapped;

    if (!map_event(event, &mapped))
        return;

    if (nrfclaw_ha_role_get() != NRFCLAW_HA_ROLE_NINALINK_NODE) {
        inc16(&m_ignored);
        return;
    }

    (void)queue_push(&mapped);
}

void nrfclaw_ninalink_event_router_process(void)
{
    queued_event_t *event;

    if (nrfclaw_ha_role_get() != NRFCLAW_HA_ROLE_NINALINK_NODE) {
        while (m_count != 0U) {
            inc16(&m_ignored);
            queue_pop();
        }
        return;
    }

    if (m_count == 0U || nrfclaw_ninalink_link_busy())
        return;

    event = &m_queue[m_head];

    if (!nrfclaw_ninalink_link_start_event(
            event->capability_id,
            event->channel,
            event->value_type,
            event->raw_value,
            EVENT_ACK_WINDOW_MS,
            EVENT_MAX_ATTEMPTS,
            EVENT_BACKOFF_MS))
        return;

    inc16(&m_sent);
    queue_pop();
}

void nrfclaw_ninalink_event_router_reset(void)
{
    m_head = 0U;
    m_tail = 0U;
    m_count = 0U;
}

void nrfclaw_ninalink_event_router_get_status(
    nrfclaw_ninalink_event_router_status_t *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    out->queued = m_count;
    out->enqueued = m_enqueued;
    out->sent = m_sent;
    out->dropped = m_dropped;
    out->ignored = m_ignored;
    out->last_capability_id = m_last_capability_id;
}
