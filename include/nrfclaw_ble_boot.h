#ifndef NRFCLAW_BLE_BOOT_H
#define NRFCLAW_BLE_BOOT_H
#include <stdbool.h>
#include <stdint.h>

/* r3.8.10: persistent Application/NDP boot advertising gate.
 * This never disables the SoftDevice itself and never disables the physical
 * P0.21 -> NUS programming plane. Default when unconfigured is enabled. */
void nrfclaw_ble_boot_init(void);
bool nrfclaw_ble_boot_ndp_enabled(void);
bool nrfclaw_ble_boot_set_ndp_enabled(bool enabled);
uint8_t nrfclaw_ble_boot_store_status(void);
#endif
