#ifndef NRFCLAW_NINALINK_BRIDGE_H
#define NRFCLAW_NINALINK_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink.h"

#define NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH 4U

typedef enum {
    NRFCLAW_NINALINK_BRIDGE_ERR_NONE = 0,
    NRFCLAW_NINALINK_BRIDGE_ERR_CORE = 1,
    NRFCLAW_NINALINK_BRIDGE_ERR_SEMANTIC = 2,
    NRFCLAW_NINALINK_BRIDGE_ERR_RADIO_STOPPED = 3,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER = 4,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TX = 5,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_RESTART = 6
} nrfclaw_ninalink_bridge_error_t;

typedef struct {
    bool active;
    uint8_t queued;
    uint16_t received;
    uint16_t valid;
    uint16_t invalid;
    uint16_t dropped;
    uint16_t radio_dropped;
    uint8_t last_error;
    uint16_t ack_sent;
} nrfclaw_ninalink_bridge_status_t;

typedef struct {
    uint8_t len;
    uint8_t data[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    int16_t rssi_x2;
    int16_t snr_x4;
} nrfclaw_ninalink_bridge_packet_t;

bool nrfclaw_ninalink_bridge_start(void);
void nrfclaw_ninalink_bridge_stop(void);
void nrfclaw_ninalink_bridge_process(void);
bool nrfclaw_ninalink_bridge_take(nrfclaw_ninalink_bridge_packet_t *out);
void nrfclaw_ninalink_bridge_get_status(nrfclaw_ninalink_bridge_status_t *out);

#endif
