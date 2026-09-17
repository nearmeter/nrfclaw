#include "nrfclaw_capability.h"

#include "nrfclaw_battery.h"
#include "nrfclaw_board.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_vib_auto.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint16_t id;
    uint8_t kind;
    uint8_t value_type;
    int8_t scale10;
    uint8_t unit;
    uint8_t behavior;
} registry_entry_t;

#define R  (NRFCLAW_CAP_BEHAVIOR_READABLE)
#define P  (NRFCLAW_CAP_BEHAVIOR_REPORTABLE)
#define E  (NRFCLAW_CAP_BEHAVIOR_EVENT_SOURCE)
#define T  (NRFCLAW_CAP_BEHAVIOR_RETAINED)

static const registry_entry_t m_registry[] = {
    { NRFCLAW_SEMCAP_BATTERY_VOLTAGE,     NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,  -3, NRFCLAW_CAP_UNIT_VOLT,             R|P|T },
    { NRFCLAW_SEMCAP_BATTERY_PERCENT,     NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U8,    0, NRFCLAW_CAP_UNIT_PERCENT,          R|P|T },
    { NRFCLAW_SEMCAP_SUPPLY_VOLTAGE,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,  -3, NRFCLAW_CAP_UNIT_VOLT,             R|P|T },

    { NRFCLAW_SEMCAP_TEMPERATURE,         NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S16,  -2, NRFCLAW_CAP_UNIT_CELSIUS,          R|P|T },
    { NRFCLAW_SEMCAP_HUMIDITY,            NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,  -2, NRFCLAW_CAP_UNIT_PERCENT,          R|P|T },
    { NRFCLAW_SEMCAP_ILLUMINANCE,         NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,   0, NRFCLAW_CAP_UNIT_LUX,              R|P|T },
    { NRFCLAW_SEMCAP_PRESSURE,            NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,   0, NRFCLAW_CAP_UNIT_PASCAL,           R|P|T },
    { NRFCLAW_SEMCAP_CO2,                 NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,   0, NRFCLAW_CAP_UNIT_PPM,              R|P|T },
    { NRFCLAW_SEMCAP_TVOC,                NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,   0, NRFCLAW_CAP_UNIT_PPB,              R|P|T },
    { NRFCLAW_SEMCAP_LEAK,                NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          R|P|E|T },

    { NRFCLAW_SEMCAP_MOTION,              NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          P|E|T },
    { NRFCLAW_SEMCAP_TAP,                 NRFCLAW_CAP_KIND_EVENT,       NRFCLAW_CAP_VALUE_ENUM8, 0, NRFCLAW_CAP_UNIT_NONE,             P|E },
    { NRFCLAW_SEMCAP_FALL,                NRFCLAW_CAP_KIND_EVENT,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          P|E },
    { NRFCLAW_SEMCAP_ACCELERATION_X,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_ACCELERATION_Y,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_ACCELERATION_Z,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_VIBRATION_RMS,       NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_VIBRATION_PEAK,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_VIBRATION_P2P,       NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,   0, NRFCLAW_CAP_UNIT_MILLI_G,          R|P|T },
    { NRFCLAW_SEMCAP_VIBRATION_FREQUENCY, NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_HERTZ,            R|P|T },
    { NRFCLAW_SEMCAP_VIBRATION_ALARM,     NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          R|P|E|T },
    { NRFCLAW_SEMCAP_WALK,                NRFCLAW_CAP_KIND_EVENT,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          P|E },

    { NRFCLAW_SEMCAP_DIGITAL_INPUT,       NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          R|P|E|T },
    { NRFCLAW_SEMCAP_HALL_STATE,          NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          P|E|T },
    { NRFCLAW_SEMCAP_COUNTER,             NRFCLAW_CAP_KIND_COUNTER,     NRFCLAW_CAP_VALUE_U32,   0, NRFCLAW_CAP_UNIT_COUNT,            R|P|T },
    { NRFCLAW_SEMCAP_QUADRATURE_POSITION, NRFCLAW_CAP_KIND_POSITION,    NRFCLAW_CAP_VALUE_S32,   0, NRFCLAW_CAP_UNIT_COUNT,            R|P|T },
    { NRFCLAW_SEMCAP_PULSE_FREQUENCY,     NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_HERTZ,            R|P|T },

    { NRFCLAW_SEMCAP_PRESENCE,            NRFCLAW_CAP_KIND_STATE,       NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          R|P|E|T },
    { NRFCLAW_SEMCAP_TRACKING_ACTIVE,     NRFCLAW_CAP_KIND_STATUS,      NRFCLAW_CAP_VALUE_BOOL,  0, NRFCLAW_CAP_UNIT_BOOLEAN,          R|P|T },

    { NRFCLAW_SEMCAP_VOLTAGE,             NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S32,  -3, NRFCLAW_CAP_UNIT_VOLT,             R|P|T },
    { NRFCLAW_SEMCAP_CURRENT,             NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S32,  -3, NRFCLAW_CAP_UNIT_AMPERE,           R|P|T },
    { NRFCLAW_SEMCAP_POWER,               NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S32,  -3, NRFCLAW_CAP_UNIT_WATT,             R|P|T },
    { NRFCLAW_SEMCAP_ENERGY,              NRFCLAW_CAP_KIND_COUNTER,     NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_WATT_HOUR,        R|P|T },
    { NRFCLAW_SEMCAP_LINE_FREQUENCY,      NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_HERTZ,            R|P|T },

    { NRFCLAW_SEMCAP_FLOW_RATE,           NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_LITER_PER_MINUTE, R|P|T },
    { NRFCLAW_SEMCAP_VOLUME,              NRFCLAW_CAP_KIND_COUNTER,     NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_LITER,            R|P|T },
    { NRFCLAW_SEMCAP_DISTANCE,            NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,  -3, NRFCLAW_CAP_UNIT_METER,            R|P|T },
    { NRFCLAW_SEMCAP_LEVEL_PERCENT,       NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16,  -2, NRFCLAW_CAP_UNIT_PERCENT,          R|P|T },
    { NRFCLAW_SEMCAP_RPM,                 NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U32,   0, NRFCLAW_CAP_UNIT_RPM,              R|P|T },
    { NRFCLAW_SEMCAP_SPEED,               NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S32,  -3, NRFCLAW_CAP_UNIT_METER_PER_SECOND, R|P|T }
};

#undef R
#undef P
#undef E
#undef T

static const registry_entry_t *registry_find(uint16_t id)
{
    uint16_t i;
    for (i = 0U; i < (uint16_t)(sizeof(m_registry) / sizeof(m_registry[0])); i++) {
        if (m_registry[i].id == id)
            return &m_registry[i];
    }
    return NULL;
}

static bool accel_supported(void)
{
#if NRFCLAW_BOARD_HAS_LIS2DH12
    return true;
#else
    return false;
#endif
}

static bool hall_supported(void)
{
#if NRFCLAW_BOARD_HAS_HALL
    return true;
#else
    return false;
#endif
}

uint16_t nrfclaw_capability_registry_count(void)
{
    return (uint16_t)(sizeof(m_registry) / sizeof(m_registry[0]));
}

bool nrfclaw_capability_registry_at(uint16_t index,
                                    nrfclaw_capability_desc_t *out)
{
    if (!out || index >= nrfclaw_capability_registry_count())
        return false;

    memset(out, 0, sizeof(*out));
    out->capability_id = m_registry[index].id;
    out->channel = 0U;
    out->kind = m_registry[index].kind;
    out->value_type = m_registry[index].value_type;
    out->scale10 = m_registry[index].scale10;
    out->unit = m_registry[index].unit;
    out->behavior_flags = m_registry[index].behavior;
    return true;
}

bool nrfclaw_capability_descriptor(uint16_t capability_id,
                                   uint8_t channel,
                                   nrfclaw_capability_desc_t *out)
{
    const registry_entry_t *e = registry_find(capability_id);
    if (!e || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->capability_id = e->id;
    out->channel = channel;
    out->kind = e->kind;
    out->value_type = e->value_type;
    out->scale10 = e->scale10;
    out->unit = e->unit;
    out->behavior_flags = e->behavior;
    return true;
}

static uint8_t lis_state_base(void)
{
    uint8_t s = 0U;
    if (accel_supported())
        s |= NRFCLAW_CAP_STATE_SUPPORTED;
    if (nrfclaw_lis2dh12_present())
        s |= NRFCLAW_CAP_STATE_PRESENT;
    return s;
}

bool nrfclaw_capability_state(uint16_t capability_id,
                              uint8_t channel,
                              uint8_t *state_flags)
{
    uint8_t s = 0U;
    nrfclaw_accel_mode_t mode;
    nrfclaw_vib_auto_status_t vst;

    if (!state_flags || !registry_find(capability_id))
        return false;

    /* B2 adapters expose only channel zero.  The ABI already supports multiple
     * channels; future providers can extend this without changing NinaLink. */
    if (channel != 0U) {
        *state_flags = 0U;
        return true;
    }

    switch (capability_id) {
        case NRFCLAW_SEMCAP_BATTERY_VOLTAGE:
#if NRFCLAW_BOARD_HAS_BATTERY
            s = NRFCLAW_CAP_STATE_SUPPORTED |
                NRFCLAW_CAP_STATE_PRESENT |
                NRFCLAW_CAP_STATE_ENABLED;
#endif
            break;

        case NRFCLAW_SEMCAP_TEMPERATURE:
#if NRFCLAW_BOARD_HAS_DS18B20
            s |= NRFCLAW_CAP_STATE_SUPPORTED;
            if (nrfclaw_ds18b20_present())
                s |= NRFCLAW_CAP_STATE_PRESENT | NRFCLAW_CAP_STATE_ENABLED;
#endif
            break;

        case NRFCLAW_SEMCAP_MOTION:
        case NRFCLAW_SEMCAP_TAP:
        case NRFCLAW_SEMCAP_FALL:
        case NRFCLAW_SEMCAP_WALK:
        case NRFCLAW_SEMCAP_ACCELERATION_X:
        case NRFCLAW_SEMCAP_ACCELERATION_Y:
        case NRFCLAW_SEMCAP_ACCELERATION_Z:
        case NRFCLAW_SEMCAP_VIBRATION_RMS:
        case NRFCLAW_SEMCAP_VIBRATION_PEAK:
        case NRFCLAW_SEMCAP_VIBRATION_P2P:
        case NRFCLAW_SEMCAP_VIBRATION_FREQUENCY:
            s = lis_state_base();
            if ((s & NRFCLAW_CAP_STATE_PRESENT) != 0U) {
                mode = nrfclaw_lis2dh12_mode();
                if (capability_id == NRFCLAW_SEMCAP_ACCELERATION_X ||
                    capability_id == NRFCLAW_SEMCAP_ACCELERATION_Y ||
                    capability_id == NRFCLAW_SEMCAP_ACCELERATION_Z) {
                    s |= NRFCLAW_CAP_STATE_ENABLED;
                } else if ((capability_id == NRFCLAW_SEMCAP_MOTION &&
                            mode == NRFCLAW_ACCEL_MODE_MOTION) ||
                           (capability_id == NRFCLAW_SEMCAP_TAP &&
                            mode == NRFCLAW_ACCEL_MODE_TAP) ||
                           (capability_id == NRFCLAW_SEMCAP_FALL &&
                            mode == NRFCLAW_ACCEL_MODE_FALL) ||
                           (capability_id == NRFCLAW_SEMCAP_WALK &&
                            mode == NRFCLAW_ACCEL_MODE_WALK) ||
                           ((capability_id == NRFCLAW_SEMCAP_VIBRATION_RMS ||
                             capability_id == NRFCLAW_SEMCAP_VIBRATION_PEAK ||
                             capability_id == NRFCLAW_SEMCAP_VIBRATION_P2P ||
                             capability_id == NRFCLAW_SEMCAP_VIBRATION_FREQUENCY) &&
                            mode == NRFCLAW_ACCEL_MODE_VIBRATION)) {
                    s |= NRFCLAW_CAP_STATE_ENABLED;
                }
                if (nrfclaw_lis2dh12_vibration_watchdog_failed() &&
                    (capability_id == NRFCLAW_SEMCAP_VIBRATION_RMS ||
                     capability_id == NRFCLAW_SEMCAP_VIBRATION_PEAK ||
                     capability_id == NRFCLAW_SEMCAP_VIBRATION_P2P ||
                     capability_id == NRFCLAW_SEMCAP_VIBRATION_FREQUENCY))
                    s |= NRFCLAW_CAP_STATE_FAULT;
            }
            break;

        case NRFCLAW_SEMCAP_VIBRATION_ALARM:
            if (nrfclaw_vib_auto_build_gate() == 1U &&
                nrfclaw_vib_auto_supported()) {
                s |= NRFCLAW_CAP_STATE_SUPPORTED;
                if (nrfclaw_lis2dh12_present())
                    s |= NRFCLAW_CAP_STATE_PRESENT;
                nrfclaw_vib_auto_get_status(&vst);
                if (vst.enabled)
                    s |= NRFCLAW_CAP_STATE_ENABLED;
                if (vst.state == NRFCLAW_VIB_AUTO_ERROR)
                    s |= NRFCLAW_CAP_STATE_FAULT;
            }
            break;

        case NRFCLAW_SEMCAP_DIGITAL_INPUT:
            /* Generic endpoint exists conceptually on boards with user GPIO,
             * but B2 has no dynamic GPIO->channel registration yet. */
            break;

        case NRFCLAW_SEMCAP_HALL_STATE:
        case NRFCLAW_SEMCAP_COUNTER:
            if (hall_supported()) {
                s |= NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT;
                if (nrfclaw_hall_active())
                    s |= NRFCLAW_CAP_STATE_ENABLED;
            }
            break;

        case NRFCLAW_SEMCAP_QUADRATURE_POSITION:
            if (hall_supported()) {
                s |= NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT;
                if (nrfclaw_hall_active() &&
                    nrfclaw_hall_mode() == NRFCLAW_HALL_MODE_QUADRATURE)
                    s |= NRFCLAW_CAP_STATE_ENABLED;
            }
            break;

        case NRFCLAW_SEMCAP_TRACKING_ACTIVE:
            s = NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT;
            if (nrfclaw_tracking_active())
                s |= NRFCLAW_CAP_STATE_ENABLED;
            break;

        default:
            /* Known registry entry, but no B2 provider on this firmware. */
            break;
    }

    *state_flags = s;
    return true;
}

static void value_clear(nrfclaw_capability_value_t *out, uint8_t type)
{
    memset(out, 0, sizeof(*out));
    out->type = type;
}

bool nrfclaw_capability_read_current(uint16_t capability_id,
                                     uint8_t channel,
                                     nrfclaw_capability_value_t *out)
{
    uint16_t cv;
    int32_t milli_c;
    nrfclaw_lis2dh12_xyz_t xyz;
    nrfclaw_accel_vibration_metrics_t vm;
    nrfclaw_vib_auto_status_t vst;

    if (!out || channel != 0U)
        return false;

    switch (capability_id) {
        case NRFCLAW_SEMCAP_BATTERY_VOLTAGE:
            if (!nrfclaw_battery_last(&cv))
                return false;
            value_clear(out, NRFCLAW_CAP_VALUE_U16);
            out->v.u16 = (uint16_t)(cv * 10U); /* centivolts -> millivolts */
            return true;

        case NRFCLAW_SEMCAP_TEMPERATURE:
            if (!nrfclaw_ds18b20_last_mC(&milli_c))
                return false;
            value_clear(out, NRFCLAW_CAP_VALUE_S16);
            out->v.s16 = (int16_t)(milli_c / 10); /* mC -> centi-C */
            return true;

        case NRFCLAW_SEMCAP_ACCELERATION_X:
        case NRFCLAW_SEMCAP_ACCELERATION_Y:
        case NRFCLAW_SEMCAP_ACCELERATION_Z:
            if (!nrfclaw_lis2dh12_read_xyz(&xyz))
                return false;
            value_clear(out, NRFCLAW_CAP_VALUE_S16);
            if (capability_id == NRFCLAW_SEMCAP_ACCELERATION_X)
                out->v.s16 = xyz.x_mg;
            else if (capability_id == NRFCLAW_SEMCAP_ACCELERATION_Y)
                out->v.s16 = xyz.y_mg;
            else
                out->v.s16 = xyz.z_mg;
            return true;

        case NRFCLAW_SEMCAP_VIBRATION_RMS:
        case NRFCLAW_SEMCAP_VIBRATION_PEAK:
        case NRFCLAW_SEMCAP_VIBRATION_P2P:
        case NRFCLAW_SEMCAP_VIBRATION_FREQUENCY:
            if (!nrfclaw_lis2dh12_vibration_metrics(&vm))
                return false;
            if (capability_id == NRFCLAW_SEMCAP_VIBRATION_FREQUENCY) {
                value_clear(out, NRFCLAW_CAP_VALUE_U32);
                /* B0 exposes zero_cross_hz as a rough frequency indicator. */
                out->v.u32 = (uint32_t)vm.zero_cross_hz * 1000UL;
            } else {
                value_clear(out, NRFCLAW_CAP_VALUE_U16);
                if (capability_id == NRFCLAW_SEMCAP_VIBRATION_RMS)
                    out->v.u16 = vm.rms_mg;
                else if (capability_id == NRFCLAW_SEMCAP_VIBRATION_PEAK)
                    out->v.u16 = vm.peak_mg;
                else
                    out->v.u16 = vm.peak_to_peak_mg;
            }
            return true;

        case NRFCLAW_SEMCAP_VIBRATION_ALARM:
            if (!nrfclaw_vib_auto_supported())
                return false;
            nrfclaw_vib_auto_get_status(&vst);
            value_clear(out, NRFCLAW_CAP_VALUE_BOOL);
            out->v.boolean = (vst.state == NRFCLAW_VIB_AUTO_ALARM);
            return true;

        case NRFCLAW_SEMCAP_COUNTER:
            if (!nrfclaw_hall_active())
                return false;
            value_clear(out, NRFCLAW_CAP_VALUE_U32);
            out->v.u32 = nrfclaw_hall_count();
            return true;

        case NRFCLAW_SEMCAP_QUADRATURE_POSITION:
            if (!nrfclaw_hall_active() ||
                nrfclaw_hall_mode() != NRFCLAW_HALL_MODE_QUADRATURE)
                return false;
            value_clear(out, NRFCLAW_CAP_VALUE_S32);
            out->v.s32 = nrfclaw_hall_position();
            return true;

        case NRFCLAW_SEMCAP_TRACKING_ACTIVE:
            value_clear(out, NRFCLAW_CAP_VALUE_BOOL);
            out->v.boolean = nrfclaw_tracking_active();
            return true;

        default:
            return false;
    }
}

uint8_t nrfclaw_capability_value_size(uint8_t value_type)
{
    switch (value_type) {
        case NRFCLAW_CAP_VALUE_BOOL:
        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_S8:
        case NRFCLAW_CAP_VALUE_ENUM8:
            return 1U;

        case NRFCLAW_CAP_VALUE_U16:
        case NRFCLAW_CAP_VALUE_S16:
            return 2U;

        case NRFCLAW_CAP_VALUE_U32:
        case NRFCLAW_CAP_VALUE_S32:
            return 4U;

        default:
            return 0U;
    }
}
