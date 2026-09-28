#include "nrfclaw_direct_sensor_control.h"

#include "nrfclaw_board.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_state.h"

/* System keys after the 16 VM/user keys and B7.6f role keys 16/17. */
#define NRFCLAW_STATE_KEY_DIRECT_SENSOR_CONTROL     18U
#define NRFCLAW_STATE_KEY_DIRECT_EVENT_SENSITIVITY 19U
#define DIRECT_SENSOR_MAGIC      0x53430100UL /* "SC", config v1 */
#define DIRECT_SENSOR_MAGIC_MASK 0xFFFFFF00UL
#define DIRECT_SENSOR_MODE_MASK  0x000000FFUL
#define DIRECT_SENS_MAGIC        0x534E5300UL /* "SNS" + preset */
#define DIRECT_SENS_MAGIC_MASK   0xFFFFFF00UL
#define DIRECT_SENS_VALUE_MASK   0x000000FFUL

static bool m_has_persisted;
static nrfclaw_direct_event_mode_t m_persisted_mode = NRFCLAW_DIRECT_SENSOR_MODE_OFF;
static bool m_has_persisted_sensitivity;
static nrfclaw_direct_event_sensitivity_t m_sensitivity = NRFCLAW_DIRECT_SENSOR_SENSITIVITY_NORMAL;

static bool mode_valid(uint32_t mode)
{
    return mode <= (uint32_t)NRFCLAW_DIRECT_SENSOR_MODE_FALL;
}

static bool sensitivity_valid(uint32_t sensitivity)
{
    return sensitivity <= (uint32_t)NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH;
}

static uint32_t encode(nrfclaw_direct_event_mode_t mode)
{
    return DIRECT_SENSOR_MAGIC |
           ((uint32_t)mode & DIRECT_SENSOR_MODE_MASK);
}

static uint32_t encode_sensitivity(nrfclaw_direct_event_sensitivity_t sensitivity)
{
    return DIRECT_SENS_MAGIC |
           ((uint32_t)sensitivity & DIRECT_SENS_VALUE_MASK);
}

static bool apply_mode(nrfclaw_direct_event_mode_t mode)
{
#if NRFCLAW_BOARD_HAS_LIS2DH12
    nrfclaw_accel_config_t cfg;
    uint16_t threshold_mg;
    uint16_t duration_ms;

    if (!nrfclaw_lis2dh12_present())
        return false;

    if (mode == NRFCLAW_DIRECT_SENSOR_MODE_OFF) {
        nrfclaw_lis2dh12_disable();
        return true;
    }

    if (mode == NRFCLAW_DIRECT_SENSOR_MODE_MOTION) {
        /* HIGH sensitivity lowers the movement threshold.  LOW additionally
         * requires two 10 Hz samples to suppress short handling impulses. */
        if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_LOW)
            return nrfclaw_lis2dh12_enable_motion(400U, 2U);
        if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH)
            return nrfclaw_lis2dh12_enable_motion(125U, 1U);
        return nrfclaw_lis2dh12_enable_motion(250U, 1U);
    }

    cfg.mode = (nrfclaw_accel_mode_t)mode;
    cfg.full_scale_g = 2U;
    cfg.duration_ms = 100U;

    if (mode == NRFCLAW_DIRECT_SENSOR_MODE_TAP) {
        /* B7.6f2l6b: keep Normal/High at the validated 40 ms click limit,
         * but make Low deliberately resistant to normal board handling:
         * 1850 mg nominal threshold and 30 ms maximum click pulse. */
        cfg.odr_hz = 100U;
        cfg.duration_ms = 40U;
        threshold_mg = 750U;
        if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_LOW) {
            threshold_mg = 1850U;
            cfg.duration_ms = 30U;
        } else if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH) {
            threshold_mg = 500U;
        }
        cfg.threshold_mg = threshold_mg;
        cfg.low_power = true;
        return nrfclaw_lis2dh12_configure(&cfg);
    }

    if (mode == NRFCLAW_DIRECT_SENSOR_MODE_FALL) {
        /* Free-fall uses XYZ-low AND semantics: a larger threshold is more
         * sensitive, while a longer required duration is less sensitive. */
        cfg.odr_hz = 10U;
        threshold_mg = 120U;
        duration_ms = 100U;
        if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_LOW) {
            threshold_mg = 80U;
            duration_ms = 200U;
        } else if (m_sensitivity == NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH) {
            threshold_mg = 200U;
            duration_ms = 100U;
        }
        cfg.threshold_mg = threshold_mg;
        cfg.duration_ms = duration_ms;
        cfg.low_power = true;
        return nrfclaw_lis2dh12_configure(&cfg);
    }

    return false;
#else
    (void)mode;
    return false;
#endif
}

void nrfclaw_direct_sensor_control_init(void)
{
    uint32_t value = 0U;

    m_has_persisted = false;
    m_persisted_mode = NRFCLAW_DIRECT_SENSOR_MODE_OFF;
    m_has_persisted_sensitivity = false;
    m_sensitivity = NRFCLAW_DIRECT_SENSOR_SENSITIVITY_NORMAL;

    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_DIRECT_SENSOR_CONTROL, &value) &&
        (value & DIRECT_SENSOR_MAGIC_MASK) == DIRECT_SENSOR_MAGIC) {
        uint32_t mode = value & DIRECT_SENSOR_MODE_MASK;
        if (mode_valid(mode)) {
            m_persisted_mode = (nrfclaw_direct_event_mode_t)mode;
            m_has_persisted = true;
        }
    }

    value = 0U;
    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_DIRECT_EVENT_SENSITIVITY, &value) &&
        (value & DIRECT_SENS_MAGIC_MASK) == DIRECT_SENS_MAGIC) {
        uint32_t sensitivity = value & DIRECT_SENS_VALUE_MASK;
        if (sensitivity_valid(sensitivity)) {
            m_sensitivity = (nrfclaw_direct_event_sensitivity_t)sensitivity;
            m_has_persisted_sensitivity = true;
        }
    }
}

bool nrfclaw_direct_sensor_control_apply_persisted(void)
{
    if (!m_has_persisted)
        return true;
    return apply_mode(m_persisted_mode);
}

bool nrfclaw_direct_sensor_control_set_persist(
    nrfclaw_direct_event_mode_t mode)
{
    if (!mode_valid((uint32_t)mode))
        return false;

#if NRFCLAW_BOARD_HAS_LIS2DH12
    if (!nrfclaw_lis2dh12_present())
        return false;
#else
    return false;
#endif

    /* A repeated choice should still re-assert runtime state after a VM or
     * diagnostic command changed the LIS mode, but must not wear Flash. */
    if (m_has_persisted && m_persisted_mode == mode)
        return apply_mode(mode);

    if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;

    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_DIRECT_SENSOR_CONTROL,
            encode(mode)))
        return false;

    m_persisted_mode = mode;
    m_has_persisted = true;

    return apply_mode(mode);
}

bool nrfclaw_direct_sensor_control_set_sensitivity_persist(
    nrfclaw_direct_event_sensitivity_t sensitivity)
{
    uint8_t runtime_mode;

    if (!sensitivity_valid((uint32_t)sensitivity))
        return false;

#if NRFCLAW_BOARD_HAS_LIS2DH12
    if (!nrfclaw_lis2dh12_present())
        return false;
#else
    return false;
#endif

    if (!m_has_persisted_sensitivity || m_sensitivity != sensitivity) {
        if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
            return false;
        if (!nrfclaw_state_persist(
                NRFCLAW_STATE_KEY_DIRECT_EVENT_SENSITIVITY,
                encode_sensitivity(sensitivity)))
            return false;
        m_sensitivity = sensitivity;
        m_has_persisted_sensitivity = true;
    }

    /* Do not steal the accelerometer back from a VM/program that has changed
     * its mode.  Re-apply only when runtime still matches the saved Direct
     * classifier. */
    runtime_mode = nrfclaw_direct_sensor_control_runtime_mode();
    if (m_has_persisted && runtime_mode == (uint8_t)m_persisted_mode)
        return apply_mode(m_persisted_mode);

    return true;
}

uint8_t nrfclaw_direct_sensor_control_runtime_mode(void)
{
#if NRFCLAW_BOARD_HAS_LIS2DH12
    if (!nrfclaw_lis2dh12_present())
        return (uint8_t)NRFCLAW_ACCEL_MODE_OFF;
    return (uint8_t)nrfclaw_lis2dh12_mode();
#else
    return (uint8_t)NRFCLAW_ACCEL_MODE_OFF;
#endif
}

nrfclaw_direct_event_mode_t nrfclaw_direct_sensor_control_persisted_mode(void)
{
    return m_persisted_mode;
}

bool nrfclaw_direct_sensor_control_has_persisted(void)
{
    return m_has_persisted;
}

nrfclaw_direct_event_sensitivity_t nrfclaw_direct_sensor_control_sensitivity(void)
{
    return m_sensitivity;
}

bool nrfclaw_direct_sensor_control_has_persisted_sensitivity(void)
{
    return m_has_persisted_sensitivity;
}

uint8_t nrfclaw_direct_sensor_control_store_status(void)
{
    return (uint8_t)nrfclaw_state_status();
}
