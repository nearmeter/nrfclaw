#ifndef NRFCLAW_NINALINK_NODE_REGISTRY_H
#define NRFCLAW_NINALINK_NODE_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink.h"

#define NRFCLAW_NINALINK_NODE_REGISTRY_CAPACITY 8U

typedef enum {
    NRFCLAW_NINALINK_NODE_DISC_NEW = 0,
    NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING = 1,
    NRFCLAW_NINALINK_NODE_DISC_CAPS_DONE = 2,
    NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING = 3,
    NRFCLAW_NINALINK_NODE_DISC_READY = 4,
    NRFCLAW_NINALINK_NODE_DISC_ERROR = 5
} nrfclaw_ninalink_node_discovery_state_t;

typedef enum {
    NRFCLAW_NINALINK_NODE_DISC_ERR_NONE = 0,
    NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_TIMEOUT = 1,
    NRFCLAW_NINALINK_NODE_DISC_ERR_COMMANDS_TIMEOUT = 2,
    NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_PROTOCOL = 3,
    NRFCLAW_NINALINK_NODE_DISC_ERR_COMMANDS_PROTOCOL = 4
} nrfclaw_ninalink_node_discovery_error_t;

typedef struct {
    uint8_t state;
    uint8_t capability_count;
    uint8_t command_count;
    uint8_t capability_registry_version;
    uint8_t command_registry_version;
    uint8_t next_capability_page;
    uint8_t next_command_index;
    uint8_t retry_count;
    uint8_t last_error;
} nrfclaw_ninalink_node_discovery_t;

typedef struct {
    bool valid;
    uint32_t node_id;
    uint16_t network_id;
    uint16_t last_sequence;
    uint8_t last_message_type;
    uint16_t seen_count;
    uint32_t last_seen_s;
    int16_t rssi_x2;
    int16_t snr_x4;
    nrfclaw_ninalink_node_discovery_t discovery;
} nrfclaw_ninalink_node_entry_t;

typedef struct {
    uint8_t count;
    uint8_t capacity;
    uint16_t updates;
    uint16_t creations;
    uint16_t evictions;
} nrfclaw_ninalink_node_registry_status_t;

void nrfclaw_ninalink_node_registry_clear(void);
void nrfclaw_ninalink_node_registry_observe(
    const nrfclaw_ninalink_frame_t *frame,
    int16_t rssi_x2,
    int16_t snr_x4,
    uint32_t now_s);
void nrfclaw_ninalink_node_registry_get_status(
    nrfclaw_ninalink_node_registry_status_t *out);
bool nrfclaw_ninalink_node_registry_get_at(
    uint8_t logical_index,
    nrfclaw_ninalink_node_entry_t *out);
bool nrfclaw_ninalink_node_registry_find(
    uint32_t node_id,
    nrfclaw_ninalink_node_entry_t *out);
bool nrfclaw_ninalink_node_registry_discovery_get(
    uint32_t node_id,
    nrfclaw_ninalink_node_discovery_t *out);
bool nrfclaw_ninalink_node_registry_discovery_set(
    uint32_t node_id,
    const nrfclaw_ninalink_node_discovery_t *value);

#endif
