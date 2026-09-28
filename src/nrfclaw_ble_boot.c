#include "nrfclaw_ble_boot.h"
#include "nrfclaw_ha_role.h"

void nrfclaw_ble_boot_init(void)
{
    nrfclaw_ha_role_init();
}

bool nrfclaw_ble_boot_ndp_enabled(void)
{
    nrfclaw_ha_role_t role = nrfclaw_ha_role_get();
    return role == NRFCLAW_HA_ROLE_DIRECT_BLE ||
           role == NRFCLAW_HA_ROLE_BRIDGE;
}

bool nrfclaw_ble_boot_set_ndp_enabled(bool enabled)
{
    /* Compatibility alias. New code should configure the explicit HA role. */
    return nrfclaw_ha_role_set_persist(
        enabled ? NRFCLAW_HA_ROLE_DIRECT_BLE : NRFCLAW_HA_ROLE_NONE);
}

uint8_t nrfclaw_ble_boot_store_status(void)
{
    return nrfclaw_ha_role_store_status();
}
