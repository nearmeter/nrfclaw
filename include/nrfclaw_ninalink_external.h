#ifndef NRFCLAW_NINALINK_EXTERNAL_H
#define NRFCLAW_NINALINK_EXTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_capability.h"

#define NRFCLAW_NINALINK_EXTERNAL_SCHEMA_VERSION 1U
#define NRFCLAW_NINALINK_EXTERNAL_NODE_VALID          0x01U
#define NRFCLAW_NINALINK_EXTERNAL_NODE_READY          0x02U
#define NRFCLAW_NINALINK_EXTERNAL_NODE_HAS_STATE      0x04U
#define NRFCLAW_NINALINK_EXTERNAL_NODE_SESSION_VALID  0x08U

typedef struct {
    uint8_t schema_version;
    uint8_t node_count;
    uint8_t state_value_count;
    uint8_t event_count;
    uint32_t oldest_event_id;
    uint32_t newest_event_id;
} nrfclaw_ninalink_external_status_t;

typedef struct {
    uint32_t node_id;
    uint8_t state_count;
    uint8_t event_count;
    uint8_t discovery_state;
    uint16_t age_s;
    int16_t rssi_x2;
    int16_t snr_x4;
    uint8_t flags;
    uint8_t last_message_type;
} nrfclaw_ninalink_external_node_t;

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
    uint8_t value_type;
    uint32_t raw_value;
    uint16_t sequence;
    uint16_t age_s;
    uint16_t update_count;
    uint8_t source_message_type;
} nrfclaw_ninalink_external_state_t;

typedef struct {
    uint32_t event_id;
    uint32_t node_id;
    uint16_t capability_id;
    uint8_t channel;
    uint8_t value_type;
    uint16_t sequence;
} nrfclaw_ninalink_external_event_meta_t;

typedef struct {
    uint32_t raw_value;
    uint16_t age_s;
} nrfclaw_ninalink_external_event_value_t;

typedef struct {
    bool known;
    uint8_t kind;
    uint8_t value_type;
    int8_t scale10;
    uint8_t unit;
    uint8_t behavior_flags;
} nrfclaw_ninalink_external_descriptor_t;

void nrfclaw_ninalink_external_get_status(
    nrfclaw_ninalink_external_status_t *out);
bool nrfclaw_ninalink_external_get_node(
    uint8_t logical_index, uint32_t now_s,
    nrfclaw_ninalink_external_node_t *out);
bool nrfclaw_ninalink_external_get_state(
    uint32_t node_id, uint8_t logical_index, uint32_t now_s,
    nrfclaw_ninalink_external_state_t *out);
bool nrfclaw_ninalink_external_get_event_after(
    uint32_t cursor, nrfclaw_ninalink_external_event_meta_t *out);
bool nrfclaw_ninalink_external_get_event_value(
    uint32_t event_id, uint32_t now_s,
    nrfclaw_ninalink_external_event_value_t *out);
void nrfclaw_ninalink_external_get_descriptor(
    uint16_t capability_id, uint8_t channel,
    nrfclaw_ninalink_external_descriptor_t *out);
#endif
