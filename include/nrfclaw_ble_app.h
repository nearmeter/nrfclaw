#ifndef NRFCLAW_BLE_APP_H
#define NRFCLAW_BLE_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_app_proto.h"

typedef enum {
    NRFCLAW_BLE_APP_OFF = 0,
    NRFCLAW_BLE_APP_ADVERTISER,
    NRFCLAW_BLE_APP_PERIPHERAL,
    NRFCLAW_BLE_APP_CENTRAL,
    NRFCLAW_BLE_APP_SCANNER
} nrfclaw_ble_app_role_t;

typedef enum {
    NRFCLAW_BLE_APP_OK = 0,
    NRFCLAW_BLE_APP_BUSY,
    NRFCLAW_BLE_APP_UNSUPPORTED,
    NRFCLAW_BLE_APP_BAD_ARG,
    NRFCLAW_BLE_APP_NOT_CONNECTED,
    NRFCLAW_BLE_APP_RESOURCES
} nrfclaw_ble_app_status_t;

void nrfclaw_ble_app_init(void);
void nrfclaw_ble_app_process(void);
void nrfclaw_ble_app_suspend(void);
void nrfclaw_ble_app_resume(void);
bool nrfclaw_ble_app_is_suspended(void);
bool nrfclaw_ble_app_connected(void);

nrfclaw_ble_app_status_t nrfclaw_ble_app_set_role(nrfclaw_ble_app_role_t role);
nrfclaw_ble_app_role_t nrfclaw_ble_app_role(void);

/* R2G R3.1: FAST -> NORMAL -> SLOW adaptive Application advertising. */
nrfclaw_ble_app_status_t nrfclaw_ble_app_adaptive_config(uint16_t fast_interval_ms,
                                                         uint32_t fast_window_ms,
                                                         uint16_t normal_interval_ms,
                                                         uint32_t normal_window_ms,
                                                         uint16_t slow_interval_ms);

nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_config(uint16_t interval_ms,
                                                    int8_t tx_power_dbm,
                                                    const uint8_t *payload,
                                                    uint8_t len);
nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_name(const uint8_t *name, uint8_t len);
nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_start(void);
nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_stop(void);
uint16_t nrfclaw_ble_app_adv_interval_ms(void);
int8_t nrfclaw_ble_app_adv_tx_power_dbm(void);

/* Stage 8A: peripheral/advertiser are real. Central/scanner API is reserved. */
nrfclaw_ble_app_status_t nrfclaw_ble_app_connect(const uint8_t addr[6]);
nrfclaw_ble_app_status_t nrfclaw_ble_app_disconnect(void);
nrfclaw_ble_app_status_t nrfclaw_ble_app_send(const nrfclaw_app_frame_t *frame);

/* Pack 02: raw NDP transport over the Application GATT TX characteristic.
 * Kept separate from the Stage-8 framed Application protocol. */
nrfclaw_ble_app_status_t nrfclaw_ble_app_send_raw(const uint8_t *data,
                                                   uint16_t len);

void nrfclaw_ble_app_on_rx(const uint8_t *data, uint16_t len);
bool nrfclaw_ble_app_last_frame(nrfclaw_app_frame_t *frame);


uint32_t nrfclaw_ble_app_tx_generation(void);


/* B7.6b: arm one connection-scoped, RAM-only low-power restart hint. */
bool nrfclaw_ble_app_next_disconnect_low_power(void);


/* B7.6d Direct Advertising Telemetry v1. */
bool nrfclaw_ble_app_telemetry_can_publish_now(void);
bool nrfclaw_ble_app_telemetry_set(const uint8_t *payload, uint8_t len);
#endif
