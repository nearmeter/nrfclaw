#ifndef NRFCLAW_CAPABILITY_H
#define NRFCLAW_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_CAPABILITY_REGISTRY_VERSION 1U

/* Semantic capability IDs.  This namespace is intentionally independent from
 * legacy NRFCLAW_CAP_* native/board feature IDs. */
typedef enum {
    NRFCLAW_SEMCAP_BATTERY_VOLTAGE        = 0x0001,
    NRFCLAW_SEMCAP_BATTERY_PERCENT        = 0x0002,
    NRFCLAW_SEMCAP_SUPPLY_VOLTAGE         = 0x0003,

    NRFCLAW_SEMCAP_TEMPERATURE            = 0x0100,
    NRFCLAW_SEMCAP_HUMIDITY               = 0x0101,
    NRFCLAW_SEMCAP_ILLUMINANCE            = 0x0102,
    NRFCLAW_SEMCAP_PRESSURE               = 0x0103,
    NRFCLAW_SEMCAP_CO2                    = 0x0104,
    NRFCLAW_SEMCAP_TVOC                   = 0x0105,
    NRFCLAW_SEMCAP_LEAK                   = 0x0106,

    NRFCLAW_SEMCAP_MOTION                 = 0x0200,
    NRFCLAW_SEMCAP_TAP                    = 0x0201,
    NRFCLAW_SEMCAP_FALL                   = 0x0202,
    NRFCLAW_SEMCAP_ACCELERATION_X         = 0x0203,
    NRFCLAW_SEMCAP_ACCELERATION_Y         = 0x0204,
    NRFCLAW_SEMCAP_ACCELERATION_Z         = 0x0205,
    NRFCLAW_SEMCAP_VIBRATION_RMS          = 0x0206,
    NRFCLAW_SEMCAP_VIBRATION_PEAK         = 0x0207,
    NRFCLAW_SEMCAP_VIBRATION_P2P          = 0x0208,
    NRFCLAW_SEMCAP_VIBRATION_FREQUENCY    = 0x0209,
    NRFCLAW_SEMCAP_VIBRATION_ALARM        = 0x020A,
    NRFCLAW_SEMCAP_WALK                   = 0x020B,
    NRFCLAW_SEMCAP_VIBRATION_EVENT       = 0x020C,


    NRFCLAW_SEMCAP_DIGITAL_INPUT          = 0x0300,
    NRFCLAW_SEMCAP_HALL_STATE             = 0x0301,
    NRFCLAW_SEMCAP_COUNTER                = 0x0302,
    NRFCLAW_SEMCAP_QUADRATURE_POSITION    = 0x0303,
    NRFCLAW_SEMCAP_PULSE_FREQUENCY        = 0x0304,

    NRFCLAW_SEMCAP_PRESENCE               = 0x0400,
    NRFCLAW_SEMCAP_TRACKING_ACTIVE        = 0x0401,

    NRFCLAW_SEMCAP_VOLTAGE                = 0x0500,
    NRFCLAW_SEMCAP_CURRENT                = 0x0501,
    NRFCLAW_SEMCAP_POWER                  = 0x0502,
    NRFCLAW_SEMCAP_ENERGY                 = 0x0503,
    NRFCLAW_SEMCAP_LINE_FREQUENCY         = 0x0504,

    NRFCLAW_SEMCAP_FLOW_RATE              = 0x0600,
    NRFCLAW_SEMCAP_VOLUME                 = 0x0601,
    NRFCLAW_SEMCAP_DISTANCE               = 0x0602,
    NRFCLAW_SEMCAP_LEVEL_PERCENT          = 0x0603,
    NRFCLAW_SEMCAP_RPM                    = 0x0604,
    NRFCLAW_SEMCAP_SPEED                  = 0x0605,
    NRFCLAW_SEMCAP_VOLUME_US_GALLON       = 0x0606,


    /* B7.6f2m6a transport-neutral device behavior controls. */
    NRFCLAW_SEMCAP_EVENT_DETECTION        = 0x0800,
    NRFCLAW_SEMCAP_EVENT_SENSITIVITY      = 0x0801,
    NRFCLAW_SEMCAP_HALL_MODE              = 0x0802,
    NRFCLAW_SEMCAP_VIBRATION_MONITORING   = 0x0803,
    NRFCLAW_SEMCAP_VIBRATION_SENSITIVITY  = 0x0804
} nrfclaw_semantic_capability_id_t;

typedef enum {
    NRFCLAW_VIB_EVENT_WARNING        = 1,
    NRFCLAW_VIB_EVENT_ALARM          = 2,
    NRFCLAW_VIB_EVENT_MACHINE_ON     = 3,
    NRFCLAW_VIB_EVENT_MACHINE_OFF    = 4,
    NRFCLAW_VIB_EVENT_LEARN_COMPLETE = 5
} nrfclaw_vibration_event_value_t;



/* Frozen NinaLink v1 numeric encodings. */
typedef enum {
    NRFCLAW_CAP_KIND_MEASUREMENT = 0x01,
    NRFCLAW_CAP_KIND_STATE       = 0x02,
    NRFCLAW_CAP_KIND_EVENT       = 0x03,
    NRFCLAW_CAP_KIND_COUNTER     = 0x04,
    NRFCLAW_CAP_KIND_POSITION    = 0x05,
    NRFCLAW_CAP_KIND_STATUS      = 0x06
} nrfclaw_capability_kind_t;

typedef enum {
    NRFCLAW_CAP_VALUE_BOOL  = 0x01,
    NRFCLAW_CAP_VALUE_U8    = 0x02,
    NRFCLAW_CAP_VALUE_S8    = 0x03,
    NRFCLAW_CAP_VALUE_U16   = 0x04,
    NRFCLAW_CAP_VALUE_S16   = 0x05,
    NRFCLAW_CAP_VALUE_U32   = 0x06,
    NRFCLAW_CAP_VALUE_S32   = 0x07,
    NRFCLAW_CAP_VALUE_ENUM8 = 0x08
} nrfclaw_capability_value_type_t;

typedef enum {
    NRFCLAW_CAP_UNIT_NONE             = 0x00,
    NRFCLAW_CAP_UNIT_BOOLEAN          = 0x01,
    NRFCLAW_CAP_UNIT_PERCENT          = 0x02,
    NRFCLAW_CAP_UNIT_VOLT             = 0x03,
    NRFCLAW_CAP_UNIT_AMPERE           = 0x04,
    NRFCLAW_CAP_UNIT_WATT             = 0x05,
    NRFCLAW_CAP_UNIT_WATT_HOUR        = 0x06,
    NRFCLAW_CAP_UNIT_CELSIUS          = 0x07,
    NRFCLAW_CAP_UNIT_PASCAL           = 0x08,
    NRFCLAW_CAP_UNIT_LUX              = 0x09,
    NRFCLAW_CAP_UNIT_PPM              = 0x0A,
    NRFCLAW_CAP_UNIT_PPB              = 0x0B,
    NRFCLAW_CAP_UNIT_MILLI_G          = 0x0C,
    NRFCLAW_CAP_UNIT_HERTZ            = 0x0D,
    NRFCLAW_CAP_UNIT_SECOND           = 0x0E,
    NRFCLAW_CAP_UNIT_METER            = 0x0F,
    NRFCLAW_CAP_UNIT_METER_PER_SECOND = 0x10,
    NRFCLAW_CAP_UNIT_LITER            = 0x11,
    NRFCLAW_CAP_UNIT_LITER_PER_MINUTE = 0x12,
    NRFCLAW_CAP_UNIT_RPM              = 0x13,
    NRFCLAW_CAP_UNIT_COUNT            = 0x14,
    NRFCLAW_CAP_UNIT_US_GALLON        = 0x15
} nrfclaw_capability_unit_t;

#define NRFCLAW_CAP_STATE_SUPPORTED 0x01U
#define NRFCLAW_CAP_STATE_PRESENT   0x02U
#define NRFCLAW_CAP_STATE_ENABLED   0x04U
#define NRFCLAW_CAP_STATE_FAULT     0x08U

#define NRFCLAW_CAP_BEHAVIOR_READABLE     0x01U
#define NRFCLAW_CAP_BEHAVIOR_REPORTABLE   0x02U
#define NRFCLAW_CAP_BEHAVIOR_EVENT_SOURCE 0x04U
#define NRFCLAW_CAP_BEHAVIOR_WRITABLE     0x08U
#define NRFCLAW_CAP_BEHAVIOR_RETAINED     0x10U

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
    uint8_t kind;
    uint8_t value_type;
    int8_t scale10;
    uint8_t unit;
    uint8_t behavior_flags;
} nrfclaw_capability_desc_t;

typedef struct {
    uint8_t type;
    union {
        bool boolean;
        uint8_t u8;
        int8_t s8;
        uint16_t u16;
        int16_t s16;
        uint32_t u32;
        int32_t s32;
    } v;
} nrfclaw_capability_value_t;

/* Global semantic registry.  registry_at() enumerates standardized semantic
 * IDs, not the capabilities supported by the current board. */
uint16_t nrfclaw_capability_registry_count(void);
bool nrfclaw_capability_registry_at(uint16_t index,
                                    nrfclaw_capability_desc_t *out);
bool nrfclaw_capability_descriptor(uint16_t capability_id,
                                   uint8_t channel,
                                   nrfclaw_capability_desc_t *out);

/* Runtime state for the current board/source.  A known semantic ID may return
 * true with state_flags==0 when this board does not provide that capability. */
bool nrfclaw_capability_state(uint16_t capability_id,
                              uint8_t channel,
                              uint8_t *state_flags);

/* Best-effort current/cached value.  This function never starts an asynchronous
 * sensor acquisition. */
/* B4.11a query semantics.
 * MOTION is a RAM-only latch: set by a MOTION event and consumed by the first
 * successful query. read_current() remains side-effect free. */
void nrfclaw_capability_motion_latch_set(void);
bool nrfclaw_capability_query_read(uint16_t capability_id,
                                   uint8_t channel,
                                   nrfclaw_capability_value_t *out);

bool nrfclaw_capability_read_current(uint16_t capability_id,
                                     uint8_t channel,
                                     nrfclaw_capability_value_t *out);


/* B4.12c synthetic TEMPERATURE provider override. */
void nrfclaw_capability_temperature_override_set(int32_t temperature_mC);
void nrfclaw_capability_temperature_override_clear(void);
bool nrfclaw_capability_temperature_override_active(void);

/* B7.6f2m6a transport-neutral semantic write result. */
typedef enum {
    NRFCLAW_CAP_WRITE_OK = 0,
    NRFCLAW_CAP_WRITE_UNSUPPORTED,
    NRFCLAW_CAP_WRITE_NOT_WRITABLE,
    NRFCLAW_CAP_WRITE_BAD_TYPE,
    NRFCLAW_CAP_WRITE_BAD_VALUE,
    NRFCLAW_CAP_WRITE_APPLY_FAILED
} nrfclaw_capability_write_result_t;

nrfclaw_capability_write_result_t
nrfclaw_capability_write(uint16_t capability_id,
                         uint8_t channel,
                         uint8_t value_type,
                         uint32_t raw_value);

uint8_t nrfclaw_capability_value_size(uint8_t value_type);

#endif
