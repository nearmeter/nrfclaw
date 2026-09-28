#include "nrfclaw_direct_adv.h"

#include "app_timer.h"

#include "nrfclaw_battery.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_native.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_vm_semantic_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef NRFCLAW_EXPERIMENTAL_VIB_AUTO
#define NRFCLAW_EXPERIMENTAL_VIB_AUTO 0
#endif

/*
 * B7.6f Direct Advertising Telemetry/Events v2.
 *
 * B7.6f v2 inserts one explicit HA transport role byte at offset 5.
 * Page 0: NC,version=2,seq,page=0,role,flags,ACTIVE,battery,temp,Hall.
 * Page 1: NC,version=2,seq,page=1,role,event,cap,channel,arg0,arg1,event_id.
 * The role lets Home Assistant classify Direct vs Bridge without asking the
 * user. HA_NINALINK_NODE does not advertise Application/NDP at all.
 *
 * Internal driver/scheduler events are not exported.  Page 1 is held for
 * ~6.5 s, covering at least three SLOW=2 s advertising opportunities without
 * switching the radio into FAST mode. A four-entry RAM queue prevents nearby
 * user events from overwriting each other. Nothing is persisted to Flash.
 */

#define DIRECT_ADV_VERSION              2U
#define DIRECT_ADV_PAGE_CORE            0U
#define DIRECT_ADV_PAGE_EVENT           1U
#define DIRECT_ADV_PAGE_ACCEL           2U
#define DIRECT_ADV_PAGE_SEMANTIC        3U
#define DIRECT_ADV_PAYLOAD_LEN         19U
#define DIRECT_ADV_SAMPLE_INTERVAL_MS 60000UL
#define DIRECT_ADV_BATTERY_EVERY        1U

#define DIRECT_ADV_FLAG_BATTERY_VALID 0x01U
#define DIRECT_ADV_FLAG_TEMP_VALID    0x02U
#define DIRECT_ADV_FLAG_HALL_VALID    0x04U
#define DIRECT_ADV_FLAG_ACCEL_VALID   0x01U

#define DIRECT_ADV_EVENT_HOLD_MS      6500UL
#define DIRECT_ADV_ACCEL_HOLD_MS      6500UL
#define DIRECT_ADV_SEMANTIC_HOLD_MS   6500UL
#define DIRECT_ADV_EVENT_QUEUE_DEPTH     4U

#define DIRECT_NATIVE_CAP_ACCEL          4U
#define DIRECT_NATIVE_CAP_HALL           8U
#define DIRECT_NATIVE_CAP_VIB_AUTO      13U

typedef struct {
    uint8_t event_type;
    uint8_t capability_id;
    uint8_t channel;
    uint32_t arg0;
    uint32_t arg1;
    uint16_t event_id;
} direct_adv_event_t;

APP_TIMER_DEF(m_direct_adv_timer);
APP_TIMER_DEF(m_direct_event_timer);
APP_TIMER_DEF(m_direct_accel_timer);
APP_TIMER_DEF(m_direct_semantic_timer);

static volatile bool m_sample_due;
static volatile bool m_event_hold_done;
static volatile bool m_accel_hold_done;
static volatile bool m_semantic_hold_done;
static bool m_wait_battery;
static bool m_wait_temperature;
static bool m_battery_valid;
static bool m_temperature_valid;
static bool m_event_timer_ready;
static bool m_event_active;
static bool m_restore_core_pending;
static bool m_accel_timer_ready;
static bool m_accel_pending;
static bool m_accel_active;
static bool m_accel_valid;
static bool m_semantic_timer_ready;
static bool m_semantic_active;
static uint16_t m_semantic_advertised_generation;

static uint8_t m_sequence;
static uint8_t m_cycle;
static uint16_t m_battery_mv;
static int16_t m_temperature_centi_c;
static int16_t m_accel_x_mg;
static int16_t m_accel_y_mg;
static int16_t m_accel_z_mg;
static uint16_t m_next_event_id;
static uint8_t m_ha_role;

static direct_adv_event_t m_event_queue[DIRECT_ADV_EVENT_QUEUE_DEPTH];
static uint8_t m_event_head;
static uint8_t m_event_tail;
static uint8_t m_event_count;
static direct_adv_event_t m_current_event;

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_i16(uint8_t *p, int16_t v)
{
    put_u16(p, (uint16_t)v);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_i32(uint8_t *p, int32_t v)
{
    put_u32(p, (uint32_t)v);
}

static uint16_t active_mask(void)
{
    uint16_t mask = 0U;
    uint8_t cap;

    for (cap = 1U; cap <= NRFCLAW_CAP_TEMPERATURE; cap++) {
        if (nrfclaw_native_capability_active(cap))
            mask |= (uint16_t)(1U << (cap - 1U));
    }
    return mask;
}

static void publish_core_force(void)
{
    uint8_t p[DIRECT_ADV_PAYLOAD_LEN];
    uint8_t flags = 0U;
    nrfclaw_hall_mode_t hall_mode = nrfclaw_hall_mode();
    int32_t hall_value = 0;

    memset(p, 0, sizeof(p));
    p[0] = 0x4EU;
    p[1] = 0x43U;
    p[2] = DIRECT_ADV_VERSION;
    p[3] = ++m_sequence;
    p[4] = DIRECT_ADV_PAGE_CORE;

    p[5] = m_ha_role;

    if (m_battery_valid) {
        flags |= DIRECT_ADV_FLAG_BATTERY_VALID;
        put_u16(&p[9], m_battery_mv);
    }

    if (m_temperature_valid) {
        flags |= DIRECT_ADV_FLAG_TEMP_VALID;
        put_i16(&p[11], m_temperature_centi_c);
    }

    p[13] = (uint8_t)hall_mode;
    p[14] = nrfclaw_hall_channel();

    if (nrfclaw_hall_active()) {
        flags |= DIRECT_ADV_FLAG_HALL_VALID;
        hall_value = (hall_mode == NRFCLAW_HALL_MODE_QUADRATURE)
            ? nrfclaw_hall_position()
            : (int32_t)nrfclaw_hall_count();
        put_i32(&p[15], hall_value);
    }

    p[6] = flags;
    put_u16(&p[7], active_mask());
    (void)nrfclaw_ble_app_telemetry_set(p, sizeof(p));
}

static void publish_accel_force(void)
{
    uint8_t p[DIRECT_ADV_PAYLOAD_LEN];
    uint8_t flags = 0U;

    memset(p, 0, sizeof(p));
    p[0] = 0x4EU;
    p[1] = 0x43U;
    p[2] = DIRECT_ADV_VERSION;
    p[3] = ++m_sequence;
    p[4] = DIRECT_ADV_PAGE_ACCEL;
    p[5] = m_ha_role;

    if (m_accel_valid) {
        flags |= DIRECT_ADV_FLAG_ACCEL_VALID;
        put_i16(&p[9], m_accel_x_mg);
        put_i16(&p[11], m_accel_y_mg);
        put_i16(&p[13], m_accel_z_mg);
    }

    p[6] = flags;
    put_u16(&p[7], active_mask());
    (void)nrfclaw_ble_app_telemetry_set(p, sizeof(p));
}

static void publish_semantic_force(nrfclaw_vm_semantic_state_t const *st)
{
    uint8_t p[DIRECT_ADV_PAYLOAD_LEN];
    if (!st)
        return;
    memset(p, 0, sizeof(p));
    p[0]=0x4EU;p[1]=0x43U;p[2]=DIRECT_ADV_VERSION;p[3]=++m_sequence;
    p[4]=DIRECT_ADV_PAGE_SEMANTIC;p[5]=m_ha_role;p[6]=st->kind;
    put_u16(&p[7],st->capability_id);p[9]=st->channel;p[10]=st->value_type;
    p[11]=(uint8_t)st->scale10;p[12]=st->unit;put_u32(&p[13],st->raw_value);
    put_u16(&p[17],st->generation);
    (void)nrfclaw_ble_app_telemetry_set(p,sizeof(p));
}

static void semantic_timer_handler(void *context)
{
    (void)context;
    m_semantic_hold_done = true;
}

static void cancel_semantic_cycle(void)
{
    if (m_semantic_timer_ready)
        (void)app_timer_stop(m_direct_semantic_timer);
    m_semantic_hold_done = false;
    m_semantic_active = false;
}

static void cancel_accel_cycle(void)
{
    if (m_accel_timer_ready)
        (void)app_timer_stop(m_direct_accel_timer);
    m_accel_hold_done = false;
    m_accel_pending = false;
    m_accel_active = false;
}

static void schedule_accel_after_core(void)
{
    if (!nrfclaw_native_capability_active(DIRECT_NATIVE_CAP_ACCEL) ||
        !m_accel_timer_ready ||
        !nrfclaw_ble_app_telemetry_can_publish_now())
        return;

    m_accel_hold_done = false;
    m_accel_pending = true;
    m_accel_active = false;
    if (app_timer_start(
            m_direct_accel_timer,
            APP_TIMER_TICKS(DIRECT_ADV_ACCEL_HOLD_MS),
            NULL) != NRF_SUCCESS) {
        m_accel_pending = false;
    }
}

static void publish_sample_complete(void)
{
    if (m_event_active || m_event_count != 0U || m_restore_core_pending || m_semantic_active)
        return;

    publish_core_force();
    schedule_accel_after_core();
}

static void publish_event(direct_adv_event_t const *event)
{
    uint8_t p[DIRECT_ADV_PAYLOAD_LEN];

    memset(p, 0, sizeof(p));
    p[0] = 0x4EU;
    p[1] = 0x43U;
    p[2] = DIRECT_ADV_VERSION;
    p[3] = ++m_sequence;
    p[4] = DIRECT_ADV_PAGE_EVENT;
    p[5] = m_ha_role;
    p[6] = event->event_type;
    p[7] = event->capability_id;
    p[8] = event->channel;
    put_u32(&p[9], event->arg0);
    put_u32(&p[13], event->arg1);
    put_u16(&p[17], event->event_id);

    (void)nrfclaw_ble_app_telemetry_set(p, sizeof(p));
}

static bool queue_push(direct_adv_event_t const *event)
{
    if (m_event_count >= DIRECT_ADV_EVENT_QUEUE_DEPTH)
        return false;

    m_event_queue[m_event_tail] = *event;
    m_event_tail = (uint8_t)((m_event_tail + 1U) %
                             DIRECT_ADV_EVENT_QUEUE_DEPTH);
    m_event_count++;
    return true;
}

static bool queue_pop(direct_adv_event_t *event)
{
    if (m_event_count == 0U || !event)
        return false;

    *event = m_event_queue[m_event_head];
    m_event_head = (uint8_t)((m_event_head + 1U) %
                             DIRECT_ADV_EVENT_QUEUE_DEPTH);
    m_event_count--;
    return true;
}

static uint16_t next_event_id(void)
{
    m_next_event_id++;
    if (m_next_event_id == 0U)
        m_next_event_id++;
    return m_next_event_id;
}

static bool map_external_event(nrfclaw_event_t const *evt,
                               direct_adv_event_t *out)
{
    if (!evt || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->arg0 = evt->arg0;
    out->arg1 = evt->arg1;

    switch (evt->type) {
        case NRFCLAW_EVT_HALL:
            out->event_type = NRFCLAW_DIRECT_EVENT_HALL;
            out->capability_id = DIRECT_NATIVE_CAP_HALL;
            out->channel = nrfclaw_hall_channel();
            break;

        case NRFCLAW_EVT_ACCEL_MOTION:
            /*
             * VIB_AUTO uses MOTION as an internal low-power machine-wake
             * primitive. Do not leak that implementation detail as a user
             * motion event; VIB_AUTO emits MACHINE_ON/WARNING/ALARM instead.
             */
            if (nrfclaw_native_capability_active(
                    DIRECT_NATIVE_CAP_VIB_AUTO))
                return false;
            out->event_type = NRFCLAW_DIRECT_EVENT_MOTION;
            out->capability_id = DIRECT_NATIVE_CAP_ACCEL;
            break;

        case NRFCLAW_EVT_ACCEL_TAP:
            out->event_type = NRFCLAW_DIRECT_EVENT_TAP;
            out->capability_id = DIRECT_NATIVE_CAP_ACCEL;
            break;

        case NRFCLAW_EVT_ACCEL_FALL:
            out->event_type = NRFCLAW_DIRECT_EVENT_FALL;
            out->capability_id = DIRECT_NATIVE_CAP_ACCEL;
            break;

        case NRFCLAW_EVT_ACCEL_WALK:
            out->event_type = NRFCLAW_DIRECT_EVENT_WALK;
            out->capability_id = DIRECT_NATIVE_CAP_ACCEL;
            break;

#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
        case NRFCLAW_EVT_VIB_AUTO_WARNING:
            out->event_type = NRFCLAW_DIRECT_EVENT_VIBRATION_WARNING;
            out->capability_id = DIRECT_NATIVE_CAP_VIB_AUTO;
            break;

        case NRFCLAW_EVT_VIB_AUTO_ALARM:
            out->event_type = NRFCLAW_DIRECT_EVENT_VIBRATION_ALARM;
            out->capability_id = DIRECT_NATIVE_CAP_VIB_AUTO;
            break;

        case NRFCLAW_EVT_VIB_AUTO_MACHINE_ON:
            out->event_type = NRFCLAW_DIRECT_EVENT_MACHINE_ON;
            out->capability_id = DIRECT_NATIVE_CAP_VIB_AUTO;
            break;

        case NRFCLAW_EVT_VIB_AUTO_MACHINE_OFF:
            out->event_type = NRFCLAW_DIRECT_EVENT_MACHINE_OFF;
            out->capability_id = DIRECT_NATIVE_CAP_VIB_AUTO;
            break;

        case NRFCLAW_EVT_VIB_AUTO_LEARN_COMPLETE:
            out->event_type =
                NRFCLAW_DIRECT_EVENT_VIBRATION_LEARN_COMPLETE;
            out->capability_id = DIRECT_NATIVE_CAP_VIB_AUTO;
            break;
#endif

        default:
            return false;
    }

    out->event_id = next_event_id();
    return true;
}

static void publish_if_complete(void)
{
    if (!m_wait_battery && !m_wait_temperature)
        publish_sample_complete();
}

static void sample_timer_handler(void *context)
{
    (void)context;
    m_sample_due = true;
}

static void event_timer_handler(void *context)
{
    (void)context;
    m_event_hold_done = true;
}

static void accel_timer_handler(void *context)
{
    (void)context;
    m_accel_hold_done = true;
}

static bool start_next_event(void)
{
    direct_adv_event_t event;

    if (!m_event_timer_ready ||
        !nrfclaw_ble_app_telemetry_can_publish_now() ||
        !queue_pop(&event))
        return false;

    m_current_event = event;
    m_event_active = true;
    m_event_hold_done = false;
    m_restore_core_pending = false;

    publish_event(&m_current_event);

    if (app_timer_start(
            m_direct_event_timer,
            APP_TIMER_TICKS(DIRECT_ADV_EVENT_HOLD_MS),
            NULL) != NRF_SUCCESS) {
        m_event_active = false;
        m_restore_core_pending = true;
        return false;
    }

    return true;
}

void nrfclaw_direct_adv_prepare_boot(uint8_t ha_role)
{
    uint8_t p[DIRECT_ADV_PAYLOAD_LEN];
    memset(p, 0, sizeof(p));
    m_ha_role = ha_role;
    p[0] = 0x4EU;
    p[1] = 0x43U;
    p[2] = DIRECT_ADV_VERSION;
    p[4] = DIRECT_ADV_PAGE_CORE;
    p[5] = m_ha_role;
    (void)nrfclaw_ble_app_telemetry_set(p, sizeof(p));
}

void nrfclaw_direct_adv_init(void)
{
    uint16_t cv;
    int32_t mc;

    m_sample_due = true;
    m_event_hold_done = false;
    m_accel_hold_done = false;
    m_semantic_hold_done = false;
    m_wait_battery = false;
    m_wait_temperature = false;
    m_battery_valid = false;
    m_temperature_valid = false;
    m_event_active = false;
    m_restore_core_pending = false;
    m_event_timer_ready = false;
    m_accel_timer_ready = false;
    m_accel_pending = false;
    m_accel_active = false;
    m_accel_valid = false;
    m_semantic_timer_ready = false;
    m_semantic_active = false;
    m_semantic_advertised_generation = 0U;
    m_accel_x_mg = 0;
    m_accel_y_mg = 0;
    m_accel_z_mg = 0;

    m_sequence = 0U;
    m_cycle = 0U;
    m_next_event_id = 0U;
    m_event_head = 0U;
    m_event_tail = 0U;
    m_event_count = 0U;
    memset(&m_current_event, 0, sizeof(m_current_event));
    memset(m_event_queue, 0, sizeof(m_event_queue));

    if (nrfclaw_battery_last(&cv)) {
        m_battery_mv = (uint16_t)(cv * 10U);
        m_battery_valid = true;
    }
    if (nrfclaw_temperature_last_mC(&mc)) {
        m_temperature_centi_c = (int16_t)(mc / 10L);
        m_temperature_valid = true;
    }

    if (app_timer_create(
            &m_direct_adv_timer,
            APP_TIMER_MODE_REPEATED,
            sample_timer_handler) == NRF_SUCCESS) {
        (void)app_timer_start(
            m_direct_adv_timer,
            APP_TIMER_TICKS(DIRECT_ADV_SAMPLE_INTERVAL_MS),
            NULL);
    }

    if (app_timer_create(
            &m_direct_event_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            event_timer_handler) == NRF_SUCCESS) {
        m_event_timer_ready = true;
    }

    if (app_timer_create(
            &m_direct_accel_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            accel_timer_handler) == NRF_SUCCESS) {
        m_accel_timer_ready = true;
    }

    if (app_timer_create(
            &m_direct_semantic_timer,
            APP_TIMER_MODE_SINGLE_SHOT,
            semantic_timer_handler) == NRF_SUCCESS) {
        m_semantic_timer_ready = true;
    }
}

void nrfclaw_direct_adv_on_event(nrfclaw_event_t const *evt)
{
    direct_adv_event_t event;

    if (!evt)
        return;

    if (evt->type == NRFCLAW_EVT_BATTERY_DONE && m_wait_battery) {
        uint32_t cv = evt->arg0;
        if (cv <= 6553U) {
            m_battery_mv = (uint16_t)(cv * 10U);
            m_battery_valid = true;
        }
        m_wait_battery = false;
        publish_if_complete();
        return;
    }

    if (evt->type == NRFCLAW_EVT_TEMPERATURE_DONE && m_wait_temperature) {
        int32_t mc = (int32_t)evt->arg0;
        int32_t cc = mc / 10L;
        if (cc >= -32768L && cc <= 32767L) {
            m_temperature_centi_c = (int16_t)cc;
            m_temperature_valid = true;
        }
        m_wait_temperature = false;
        publish_if_complete();
        return;
    }

    /*
     * Export only the explicit user-facing allowlist above. Internal
     * BATTERY/DS18B20 steps, RTC, BLE, LoRa, VM and watchdog events never
     * become part of the public Direct event protocol.
     */
    if (m_event_timer_ready && map_external_event(evt, &event))
        (void)queue_push(&event);
}

void nrfclaw_direct_adv_process(void)
{
    bool started = false;

    /*
     * Never compete with P0.21/NUS, a live Application connection or another
     * Application role. Queued events stay in RAM until Direct advertising
     * returns to an idle SLOW publication point.
     */
    if (nrfclaw_ble_app_is_suspended() ||
        nrfclaw_ble_app_connected() ||
        nrfclaw_ble_app_role() != NRFCLAW_BLE_APP_PERIPHERAL)
        return;

    if (m_semantic_active && m_semantic_hold_done) {
        m_semantic_hold_done = false;
        m_semantic_active = false;
        m_restore_core_pending = true;
    }

    if (m_event_active && m_event_hold_done) {
        m_event_hold_done = false;
        m_event_active = false;
        m_restore_core_pending = true;
    }

    if (!m_event_active && m_event_count != 0U) {
        /* User events always outrank periodic acceleration telemetry. */
        cancel_accel_cycle();
        cancel_semantic_cycle();
        if (start_next_event())
            return;
    }

    if (!m_event_active &&
        m_event_count == 0U &&
        m_restore_core_pending &&
        nrfclaw_ble_app_telemetry_can_publish_now()) {
        m_restore_core_pending = false;
        publish_core_force();
    }

    if (m_event_active || m_event_count != 0U || m_restore_core_pending)
        return;

    if (!m_semantic_active && m_semantic_timer_ready &&
        nrfclaw_ble_app_telemetry_can_publish_now()) {
        nrfclaw_vm_semantic_state_t semantic;
        if (nrfclaw_vm_semantic_state_snapshot(&semantic) &&
            semantic.generation != m_semantic_advertised_generation) {
            cancel_accel_cycle();
            publish_semantic_force(&semantic);
            m_semantic_advertised_generation = semantic.generation;
            m_semantic_active = true;
            m_semantic_hold_done = false;
            if (app_timer_start(m_direct_semantic_timer,
                                APP_TIMER_TICKS(DIRECT_ADV_SEMANTIC_HOLD_MS),
                                NULL) != NRF_SUCCESS) {
                m_semantic_active = false;
                m_restore_core_pending = true;
            }
            return;
        }
    }

    if (m_semantic_active)
        return;

    if (m_accel_pending && m_accel_hold_done &&
        nrfclaw_ble_app_telemetry_can_publish_now()) {
        m_accel_hold_done = false;
        m_accel_pending = false;
        m_accel_active = true;
        publish_accel_force();
        if (app_timer_start(
                m_direct_accel_timer,
                APP_TIMER_TICKS(DIRECT_ADV_ACCEL_HOLD_MS),
                NULL) != NRF_SUCCESS) {
            m_accel_active = false;
            publish_core_force();
        }
        return;
    }

    if (m_accel_active && m_accel_hold_done &&
        nrfclaw_ble_app_telemetry_can_publish_now()) {
        m_accel_hold_done = false;
        m_accel_active = false;
        publish_core_force();
        return;
    }

    if (m_accel_pending || m_accel_active)
        return;

    if (!m_sample_due)
        return;

    m_sample_due = false;
    m_cycle++;
    m_wait_battery = false;
    m_wait_temperature = false;
    m_accel_valid = false;

    if (nrfclaw_native_capability_active(DIRECT_NATIVE_CAP_ACCEL)) {
        int16_t x, y, z;
        if (nrfclaw_native_accel_xyz(&x, &y, &z) == NRFCLAW_NATIVE_OK) {
            m_accel_x_mg = x;
            m_accel_y_mg = y;
            m_accel_z_mg = z;
            m_accel_valid = true;
        }
    }

    if (m_cycle == 1U || (m_cycle % DIRECT_ADV_BATTERY_EVERY) == 0U) {
        if (!nrfclaw_battery_busy() && nrfclaw_battery_start()) {
            m_wait_battery = true;
            started = true;
        }
    }

    if (!nrfclaw_temperature_busy() && nrfclaw_temperature_start()) {
        m_wait_temperature = true;
        started = true;
    } else {
        m_temperature_valid = false;
    }

    if (!started)
        publish_sample_complete();
}
