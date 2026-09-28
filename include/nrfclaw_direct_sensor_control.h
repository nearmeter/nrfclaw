#ifndef NRFCLAW_DIRECT_SENSOR_CONTROL_H
#define NRFCLAW_DIRECT_SENSOR_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Direct sensor/event control.
 *
 * The LIS2DH12 has one interrupt/event mode at a time.  Motion/Tap/Fall are
 * therefore one exclusive mode.  B7.6f2k3 adds a small three-level
 * sensitivity preset rather than exposing raw mg/ms register tuning in HA.
 */
typedef enum {
    NRFCLAW_DIRECT_SENSOR_MODE_OFF    = 0,
    NRFCLAW_DIRECT_SENSOR_MODE_MOTION = 1,
    NRFCLAW_DIRECT_SENSOR_MODE_TAP    = 2,
    NRFCLAW_DIRECT_SENSOR_MODE_FALL   = 3
} nrfclaw_direct_event_mode_t;

typedef enum {
    NRFCLAW_DIRECT_SENSOR_SENSITIVITY_LOW    = 0,
    NRFCLAW_DIRECT_SENSOR_SENSITIVITY_NORMAL = 1,
    NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH   = 2
} nrfclaw_direct_event_sensitivity_t;

void nrfclaw_direct_sensor_control_init(void);

/* Apply the saved Direct preset, if one exists.  No-op when unconfigured. */
bool nrfclaw_direct_sensor_control_apply_persisted(void);

/* Persist + apply one of OFF/MOTION/TAP/FALL. */
bool nrfclaw_direct_sensor_control_set_persist(
    nrfclaw_direct_event_mode_t mode);

/* Persist a user-facing LOW/NORMAL/HIGH classifier preset. */
bool nrfclaw_direct_sensor_control_set_sensitivity_persist(
    nrfclaw_direct_event_sensitivity_t sensitivity);

/* Actual LIS2DH12 runtime mode, including program-controlled WALK/VIBRATION. */
uint8_t nrfclaw_direct_sensor_control_runtime_mode(void);

/* Saved HA preset. OFF is returned when no preset has ever been stored. */
nrfclaw_direct_event_mode_t nrfclaw_direct_sensor_control_persisted_mode(void);
bool nrfclaw_direct_sensor_control_has_persisted(void);

/* NORMAL is the product default until the user explicitly chooses otherwise. */
nrfclaw_direct_event_sensitivity_t nrfclaw_direct_sensor_control_sensitivity(void);
bool nrfclaw_direct_sensor_control_has_persisted_sensitivity(void);

/* Mirrors nrfclaw_state_status(): 0 idle, 1 saving, 2 error. */
uint8_t nrfclaw_direct_sensor_control_store_status(void);

#endif
