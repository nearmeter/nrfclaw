#include "nrfclaw_ha_role.h"

#include "nrfclaw_board.h"
#include "nrfclaw_state.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_direct_adv.h"
#include "nrfclaw_direct_sensor_control.h"
#include "nrfclaw_direct_hall_control.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_telemetry.h"
#include "nrfclaw_ha_ninalink_node.h"

#include <stddef.h>

#define NRFCLAW_STATE_KEY_LEGACY_BLE_NDP_BOOT 16U
#define NRFCLAW_STATE_KEY_HA_ROLE             17U
#define LEGACY_BLE_BOOT_MAGIC_ENABLED  0x4E445001UL
#define HA_ROLE_MAGIC_BASE             0x48415200UL /* legacy: 'HAR' + role */
#define HA_CONFIG_MAGIC_BASE           0x48430000UL /* 'HC' + packed role/period */
#define HA_CONFIG_MAGIC_MASK           0xFFFFC000UL
#define HA_CONFIG_ROLE_MASK            0x00000003UL
#define HA_CONFIG_PERIOD_SHIFT         2U
#define HA_CONFIG_PERIOD_MASK          0x00000FFFUL

static nrfclaw_ha_role_t m_role = NRFCLAW_HA_ROLE_NONE;
static uint16_t m_node_period_s = NRFCLAW_HA_NODE_DEFAULT_PERIOD_S;
static bool m_role_key_present;

static bool valid_role(uint32_t role)
{
    return role <= (uint32_t)NRFCLAW_HA_ROLE_BRIDGE;
}

static bool role_supported_by_board(nrfclaw_ha_role_t role)
{
#if NRFCLAW_BOARD_HAS_LORA
    (void)role;
    return true;
#else
    return role != NRFCLAW_HA_ROLE_NINALINK_NODE &&
           role != NRFCLAW_HA_ROLE_BRIDGE;
#endif
}

static bool valid_node_period(uint32_t period_s)
{
    return period_s >= NRFCLAW_HA_NODE_MIN_PERIOD_S &&
           period_s <= NRFCLAW_HA_NODE_MAX_PERIOD_S;
}

static uint32_t encode_config(nrfclaw_ha_role_t role,
                              uint16_t period_s)
{
    return HA_CONFIG_MAGIC_BASE |
           ((uint32_t)period_s << HA_CONFIG_PERIOD_SHIFT) |
           ((uint32_t)role & HA_CONFIG_ROLE_MASK);
}

void nrfclaw_ha_role_init(void)
{
    uint32_t value = 0U;

    m_role = NRFCLAW_HA_ROLE_NONE;
    m_node_period_s = NRFCLAW_HA_NODE_DEFAULT_PERIOD_S;
    m_role_key_present = false;

    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_HA_ROLE, &value)) {
        if ((value & HA_CONFIG_MAGIC_MASK) == HA_CONFIG_MAGIC_BASE) {
            uint32_t role = value & HA_CONFIG_ROLE_MASK;
            uint32_t period_s =
                (value >> HA_CONFIG_PERIOD_SHIFT) & HA_CONFIG_PERIOD_MASK;
            if (valid_role(role) &&
                role_supported_by_board((nrfclaw_ha_role_t)role) &&
                valid_node_period(period_s)) {
                m_role = (nrfclaw_ha_role_t)role;
                m_node_period_s = (uint16_t)period_s;
                m_role_key_present = true;
                return;
            }
        }

        /* B7.6f compatibility: old role-only records had no interval. */
        if ((value & 0xFFFFFF00UL) == HA_ROLE_MAGIC_BASE &&
            valid_role(value & 0xFFU) &&
            role_supported_by_board((nrfclaw_ha_role_t)(value & 0xFFU))) {
            m_role = (nrfclaw_ha_role_t)(value & 0xFFU);
            m_node_period_s = NRFCLAW_HA_NODE_DEFAULT_PERIOD_S;
            m_role_key_present = true;
            return;
        }
    }

    /* Compatibility migration is read-only: an explicitly ENABLED legacy
     * NDP boot gate behaves as DIRECT_BLE. An absent legacy key or an explicit
     * DISABLED value maps to the new low-power default NONE. The new key is
     * written only when configuration is next changed, avoiding flash writes
     * merely because the device booted. */
    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_LEGACY_BLE_NDP_BOOT, &value) &&
        value == LEGACY_BLE_BOOT_MAGIC_ENABLED) {
        m_role = NRFCLAW_HA_ROLE_DIRECT_BLE;
    }
}

nrfclaw_ha_role_t nrfclaw_ha_role_get(void)
{
    return m_role;
}

uint16_t nrfclaw_ha_role_node_period_get(void)
{
    return m_node_period_s;
}

bool nrfclaw_ha_role_set_persist(nrfclaw_ha_role_t role)
{
    if (!valid_role((uint32_t)role) || !role_supported_by_board(role))
        return false;

    if (m_role_key_present && m_role == role)
        return true;

    if (!nrfclaw_state_persist(NRFCLAW_STATE_KEY_HA_ROLE,
                               encode_config(role, m_node_period_s)))
        return false;

    m_role = role;
    m_role_key_present = true;
    return true;
}

static bool start_application_ble(nrfclaw_ha_role_t role)
{
#if NRFCLAW_HA_NATIVE_ENABLE
    nrfclaw_ble_app_status_t st;

    nrfclaw_ble_app_resume();

    st = nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_PERIPHERAL);
    if (st != NRFCLAW_BLE_APP_OK)
        return false;

    st = nrfclaw_ble_app_adaptive_config(
        NRFCLAW_HA_ADV_FAST_INTERVAL_MS,
        NRFCLAW_HA_ADV_FAST_WINDOW_MS,
        NRFCLAW_HA_ADV_NORMAL_INTERVAL_MS,
        NRFCLAW_HA_ADV_NORMAL_WINDOW_MS,
        NRFCLAW_HA_ADV_SLOW_INTERVAL_MS);
    if (st != NRFCLAW_BLE_APP_OK)
        return false;

    /* Role is present in the very first packet. This prevents a BRIDGE from
     * briefly looking like a legacy Direct device during HA discovery. */
    nrfclaw_direct_adv_prepare_boot((uint8_t)role);

    st = nrfclaw_ble_app_adv_config(
        NRFCLAW_HA_ADV_FAST_INTERVAL_MS,
        NRFCLAW_HA_ADV_TX_POWER_DBM,
        NULL,
        0U);
    if (st != NRFCLAW_BLE_APP_OK)
        return false;

    st = nrfclaw_ble_app_adv_start();
    return st == NRFCLAW_BLE_APP_OK;
#else
    (void)role;
    return false;
#endif
}

bool nrfclaw_ha_role_apply_runtime(nrfclaw_ha_role_t role)
{
    if (!valid_role((uint32_t)role) || !role_supported_by_board(role))
        return false;

    /* Roles are exclusive. Stop every autonomous HA transport before taking
     * ownership for the selected one. Legacy B4.12 telemetry is included so a
     * prior lab BOOT program cannot remain active behind a new HA role. */
    nrfclaw_ha_ninalink_node_stop();
    nrfclaw_ninalink_telemetry_stop();
    nrfclaw_ninalink_bridge_stop();

    if (role == NRFCLAW_HA_ROLE_NONE ||
        role == NRFCLAW_HA_ROLE_NINALINK_NODE) {
        nrfclaw_ble_app_suspend();
        if (nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_OFF) !=
            NRFCLAW_BLE_APP_OK)
            return false;
    }

    if (role == NRFCLAW_HA_ROLE_NONE) {
        ((void)0);
        return true;
    }

    if (role == NRFCLAW_HA_ROLE_NINALINK_NODE) {
        if (!nrfclaw_ha_ninalink_node_start(m_node_period_s))
            return false;
        ((void)0);
        return true;
    }

    if (role == NRFCLAW_HA_ROLE_DIRECT_BLE &&
        !nrfclaw_direct_sensor_control_apply_persisted()) {
        ((void)0);
    }
    if ((role == NRFCLAW_HA_ROLE_DIRECT_BLE ||
         role == NRFCLAW_HA_ROLE_NINALINK_NODE) &&
        !nrfclaw_direct_hall_control_apply_persisted()) {
        ((void)0);
    }

    if (!start_application_ble(role))
        return false;

    if (role == NRFCLAW_HA_ROLE_BRIDGE) {
        if (!nrfclaw_ninalink_bridge_start())
            return false;
        ((void)0);
    } else {
        ((void)0);
    }

    return true;
}

bool nrfclaw_ha_role_configure(nrfclaw_ha_role_t role)
{
    if (!nrfclaw_ha_role_set_persist(role))
        return false;
    return nrfclaw_ha_role_apply_runtime(role);
}

bool nrfclaw_ha_role_configure_node(uint16_t period_s)
{
    if (!role_supported_by_board(NRFCLAW_HA_ROLE_NINALINK_NODE) ||
        !valid_node_period(period_s))
        return false;

    if (!(m_role_key_present &&
          m_role == NRFCLAW_HA_ROLE_NINALINK_NODE &&
          m_node_period_s == period_s)) {
        if (!nrfclaw_state_persist(
                NRFCLAW_STATE_KEY_HA_ROLE,
                encode_config(NRFCLAW_HA_ROLE_NINALINK_NODE,
                              period_s)))
            return false;
        m_role = NRFCLAW_HA_ROLE_NINALINK_NODE;
        m_node_period_s = period_s;
        m_role_key_present = true;
    }

    return nrfclaw_ha_role_apply_runtime(
        NRFCLAW_HA_ROLE_NINALINK_NODE);
}

uint8_t nrfclaw_ha_role_store_status(void)
{
    return (uint8_t)nrfclaw_state_status();
}
