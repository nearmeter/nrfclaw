#ifndef NRFCLAW_NINALINK_BRIDGE_H
#define NRFCLAW_NINALINK_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink.h"
#include "nrfclaw_ninalink_command_discovery.h"
#include "nrfclaw_ninalink_capability_discovery.h"

#define NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH 4U
#define NRFCLAW_NINALINK_BRIDGE_DUP_CACHE   8U

typedef enum {
    NRFCLAW_NINALINK_BRIDGE_ERR_NONE = 0,
    NRFCLAW_NINALINK_BRIDGE_ERR_CORE = 1,
    NRFCLAW_NINALINK_BRIDGE_ERR_SEMANTIC = 2,
    NRFCLAW_NINALINK_BRIDGE_ERR_RADIO_STOPPED = 3,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TIMER = 4,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_TX = 5,
    NRFCLAW_NINALINK_BRIDGE_ERR_ACK_RESTART = 6
} nrfclaw_ninalink_bridge_error_t;

typedef enum {
    NRFCLAW_NINALINK_APP_DL_IDLE = 0,
    NRFCLAW_NINALINK_APP_DL_PENDING = 1,
    NRFCLAW_NINALINK_APP_DL_WAIT_RESULT = 2,
    NRFCLAW_NINALINK_APP_DL_DONE = 3
} nrfclaw_ninalink_app_dl_state_t;

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
    uint16_t duplicates;
    uint16_t ack_test_dropped;
    bool drop_next_ack;
} nrfclaw_ninalink_bridge_status_t;

typedef struct {
    bool pending;
    uint32_t target_node;
    uint16_t command_seq;
    bool requested_value;
    uint8_t state;
    uint8_t result;
    uint16_t sent_count;
    uint16_t completed_count;
    uint8_t timeout_count;
} nrfclaw_ninalink_app_dl_status_t;

#define NRFCLAW_NINALINK_COMMAND_DATA_MAX 16U

typedef struct {
    bool pending;
    uint32_t target_node;
    uint16_t command_seq;
    uint16_t command_id;
    uint8_t state;
    uint8_t result;
    uint8_t result_len;
    uint8_t result_data[NRFCLAW_NINALINK_COMMAND_DATA_MAX];
    uint16_t sent_count;
    uint16_t completed_count;
    uint8_t timeout_count;
} nrfclaw_ninalink_command_dl_status_t;

typedef struct {
    bool pending;
    uint32_t target_node;
    uint16_t request_seq;
    uint8_t start_index;
    uint8_t state;
    uint8_t registry_version;
    uint8_t total_count;
    uint8_t count;
    nrfclaw_ninalink_command_descriptor_t
        descriptors[NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX];
    uint16_t sent_count;
    uint16_t completed_count;
    uint8_t timeout_count;
} nrfclaw_ninalink_command_discovery_status_t;

typedef struct {
    bool pending;
    uint32_t target_node;
    uint16_t request_seq;
    uint8_t page_index;
    uint8_t state;
    uint8_t registry_version;
    uint8_t count;
    bool more;
    nrfclaw_ninalink_capability_descriptor_t
        descriptors[NRFCLAW_NINALINK_CAPABILITY_DISCOVERY_PAGE_MAX];
    uint16_t sent_count;
    uint16_t completed_count;
    uint8_t timeout_count;
} nrfclaw_ninalink_capability_discovery_status_t;

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
void nrfclaw_ninalink_bridge_drop_next_ack(void);

bool nrfclaw_ninalink_bridge_queue_tracking(uint32_t target_node, bool active);
void nrfclaw_ninalink_bridge_get_app_status(
    nrfclaw_ninalink_app_dl_status_t *out);

bool nrfclaw_ninalink_bridge_queue_command(
    uint32_t target_node,
    uint16_t command_id,
    const uint8_t *args,
    uint8_t arg_len);
void nrfclaw_ninalink_bridge_get_command_status(
    nrfclaw_ninalink_command_dl_status_t *out);

bool nrfclaw_ninalink_bridge_queue_command_discovery(
    uint32_t target_node,
    uint8_t start_index);
void nrfclaw_ninalink_bridge_get_command_discovery_status(
    nrfclaw_ninalink_command_discovery_status_t *out);

bool nrfclaw_ninalink_bridge_queue_capability_discovery(
    uint32_t target_node,
    uint8_t page_index);
void nrfclaw_ninalink_bridge_get_capability_discovery_status(
    nrfclaw_ninalink_capability_discovery_status_t *out);

#endif
