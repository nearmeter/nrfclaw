#ifndef NRFCLAW_BLE_BOOT_H
#define NRFCLAW_BLE_BOOT_H
#include <stdbool.h>
#include <stdint.h>

/* Compatibility wrapper for the former r3.8.10 NDP boot gate.
 * B7.6f owns boot behavior through nrfclaw_ha_role. Fresh devices now default
 * to HA_NONE / NDP OFF. P0.21 -> NUS remains available in every role. */
void nrfclaw_ble_boot_init(void);
bool nrfclaw_ble_boot_ndp_enabled(void);
bool nrfclaw_ble_boot_set_ndp_enabled(bool enabled);
uint8_t nrfclaw_ble_boot_store_status(void);
#endif
