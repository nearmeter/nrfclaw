#ifndef NRFCLAW_BLE_H
#define NRFCLAW_BLE_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_BLE_DISCOVERY_TIMEOUT_S   600U
#define NRFCLAW_BLE_SESSION_IDLE_TIMEOUT_S 300U

void nrfclaw_ble_init(void);
void nrfclaw_ble_programming_open(uint32_t seconds);
void nrfclaw_ble_programming_close(void);
bool nrfclaw_ble_is_connected(void);
bool nrfclaw_ble_programming_active(void);
void nrfclaw_ble_process(void);

/* Plane-specific GAP names make NDP/Application and NUS/programming obvious. */
void nrfclaw_ble_set_gap_name_ndp(void);
void nrfclaw_ble_set_gap_name_nus(void);

/* Single advertising resource shared by NUS and Application BLE. */
uint8_t nrfclaw_ble_shared_adv_handle(void);

#endif
