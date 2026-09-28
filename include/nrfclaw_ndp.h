#ifndef NRFCLAW_NDP_H
#define NRFCLAW_NDP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * nRFClaw NDP v1 public wire contract.
 *
 * Request:  flags:u8 opcode:u8 seq:u8 payload_len:u8 payload...
 * Response: flags:u8 opcode:u8 seq:u8 payload_len:u8 status:u8 payload...
 *
 * The validated BLE Application GATT transport uses ATT_MTU=23, therefore
 * requests and responses used by HA remain <=20 bytes unless fragmentation is
 * introduced by a future protocol revision.
 */
#define NRFCLAW_NDP_MAX_FRAME 64U

/* Session response flags. */
#define NRFCLAW_NDP_FLAG_RESPONSE 0x01U
#define NRFCLAW_NDP_FLAG_ERROR    0x04U

/* Stable NDP v1 opcodes. */
typedef enum {
    NRFCLAW_NDP_INFO             = 32,
    NRFCLAW_NDP_CAPS             = 33,
    NRFCLAW_NDP_STATUS           = 34,
    NRFCLAW_NDP_BOARD_INFO       = 35,
    NRFCLAW_NDP_BOARD_MEMORY     = 36,
    NRFCLAW_NDP_BOARD_RESET      = 37,
    NRFCLAW_NDP_BOARD_BOOTLOADER = 38,
    NRFCLAW_NDP_STATE_GET        = 48,
    NRFCLAW_NDP_SENSOR_READ      = 52,
    NRFCLAW_NDP_RADIO_GET        = 56,
    NRFCLAW_NDP_RADIO_SET        = 57,
    NRFCLAW_NDP_HALL_CONFIG      = 58,
    NRFCLAW_NDP_ERROR            = 63,
    NRFCLAW_NDP_AUTH_BEGIN       = 64,
    NRFCLAW_NDP_AUTH_FINISH      = 65,
    NRFCLAW_NDP_AUTH_LOGOUT      = 66,
    NRFCLAW_NDP_ACCESS_STATUS    = 67,
    NRFCLAW_NDP_ACCEL_CONFIG     = 68,
    NRFCLAW_NDP_VIB_INFO         = 69,
    NRFCLAW_NDP_VIB_READ         = 70,
    NRFCLAW_NDP_ACCEL_PROBE      = 71,
    NRFCLAW_NDP_VIB_MODEL_SET    = 72,
    NRFCLAW_NDP_VIB_HEALTH       = 73,
    NRFCLAW_NDP_VIB_AUTO_START   = 74,
    NRFCLAW_NDP_VIB_AUTO_STOP    = 75,
    NRFCLAW_NDP_VIB_AUTO_STATUS  = 76,
    NRFCLAW_NDP_VIB_AUTO_CONFIG  = 77,
    NRFCLAW_NDP_VIB_AUTO_BASELINE= 78,
    NRFCLAW_NDP_VIB_AUTO_RESET   = 79,
    /* Physical-NUS-only owner key management. */
    NRFCLAW_NDP_KEY_GENERATE     = 80,
    NRFCLAW_NDP_KEY_GET          = 81,
    NRFCLAW_NDP_KEY_STATUS       = 82,
    /* r3.8.10 physical-NUS-only boot control for Application/NDP advertising. */
    NRFCLAW_NDP_BLE_BOOT_CONTROL = 83,
    /* 84 retired: historical Beacon/Broadcaster hardware test control. */
    /* 85..86 retired: historical direct LoRa diagnostic endpoints. */
    /* r3.8.18 extended persistent LoRa profile; legacy 56/57 remain frozen. */
    NRFCLAW_NDP_RADIO_GET_EXT      = 87,
    NRFCLAW_NDP_RADIO_SET_EXT      = 88,
    /* 89 retired: historical NinaLink lab uplink endpoint. */
    /* B4.2 physical-NUS bridge control / validated RX queue. */
    NRFCLAW_NDP_NINALINK_BRIDGE    = 90,
    /* 91 retired: historical NinaLink node/link hardware gate endpoint. */
    /* B7.6b Application-only one-shot intentional disconnect hint. */
    NRFCLAW_NDP_BLE_DISCONNECT_HINT = 92,
    NRFCLAW_NDP_HA_ROLE_CONTROL      = 93,
    /* B7.6f2k2 Direct accelerometer event preset GET/SET. */
    NRFCLAW_NDP_DIRECT_SENSOR_CONTROL = 94,
    /* B7.6f2k3 user-facing LOW/NORMAL/HIGH event sensitivity. */
    NRFCLAW_NDP_DIRECT_EVENT_SENSITIVITY = 95,
    /* B7.6f2k3 persistent Hall behavior + reset. */
    NRFCLAW_NDP_DIRECT_HALL_CONTROL = 96,
    /* B7.6f2k4 read-only current VM semantic state. */
    NRFCLAW_NDP_DIRECT_SEMANTIC_STATE = 97,
    /* B7.6f2l1 physical-NUS-only persistent NinaLink network ID. */
    NRFCLAW_NDP_NINALINK_NETWORK = 98
} nrfclaw_ndp_opcode_t;

/* Stable NDP v1 status values. */
typedef enum {
    NRFCLAW_NDP_OK           = 0,
    NRFCLAW_NDP_BAD_LENGTH   = 1,
    NRFCLAW_NDP_BAD_ARG      = 2,
    NRFCLAW_NDP_UNSUPPORTED  = 3,
    NRFCLAW_NDP_BUSY         = 4,
    NRFCLAW_NDP_UNAUTHORIZED = 5,
    NRFCLAW_NDP_FORBIDDEN    = 6,
    NRFCLAW_NDP_AUTH_FAILED  = 7
} nrfclaw_ndp_status_t;

/* SENSOR_READ selector values. */
typedef enum {
    NRFCLAW_NDP_SENSOR_BATTERY      = 1,
    NRFCLAW_NDP_SENSOR_HALL         = 2,
    NRFCLAW_NDP_SENSOR_ACCEL_XYZ    = 3,
    NRFCLAW_NDP_SENSOR_DS18B20      = 4,
    NRFCLAW_NDP_SENSOR_ACCEL_METRICS= 5,
    NRFCLAW_NDP_SENSOR_TEMPERATURE  = 6
} nrfclaw_ndp_sensor_t;

bool nrfclaw_ndp_is_session_frame(uint8_t const *data,uint16_t len);
bool nrfclaw_ndp_handle_session(uint8_t const *data,uint16_t len,uint8_t *out,uint16_t *out_len);

/*
 * Transport-aware entry point. Application GATT calls this with
 * enforce_auth=true. Physical-button NUS calls it with false, because the
 * physical programming window is the trust boundary for that plane.
 */
bool nrfclaw_ndp_handle_session_transport(uint8_t const *data,uint16_t len,
                                           uint8_t *out,uint16_t *out_len,
                                           bool enforce_auth);

#endif
