#include "nrfclaw_ble_boot.h"
#include "nrfclaw_state.h"

/* Reserve the last state-journal key for the Application/NDP boot gate. */
#define NRFCLAW_STATE_KEY_BLE_NDP_BOOT 16U
#define BLE_BOOT_MAGIC_ENABLED  0x4E445001UL
#define BLE_BOOT_MAGIC_DISABLED 0x4E445000UL

static bool m_enabled = true;

void nrfclaw_ble_boot_init(void)
{
    uint32_t value = 0U;
    m_enabled = true;
    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_BLE_NDP_BOOT, &value)) {
        if (value == BLE_BOOT_MAGIC_DISABLED) m_enabled = false;
        else if (value == BLE_BOOT_MAGIC_ENABLED) m_enabled = true;
    }
}

bool nrfclaw_ble_boot_ndp_enabled(void){ return m_enabled; }

bool nrfclaw_ble_boot_set_ndp_enabled(bool enabled)
{
    uint32_t value = enabled ? BLE_BOOT_MAGIC_ENABLED : BLE_BOOT_MAGIC_DISABLED;
    if (!nrfclaw_state_persist(NRFCLAW_STATE_KEY_BLE_NDP_BOOT, value))
        return false;
    m_enabled = enabled;
    return true;
}

uint8_t nrfclaw_ble_boot_store_status(void)
{
    return (uint8_t)nrfclaw_state_status();
}
