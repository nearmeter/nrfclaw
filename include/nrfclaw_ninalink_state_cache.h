#ifndef NRFCLAW_NINALINK_STATE_CACHE_H
#define NRFCLAW_NINALINK_STATE_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_ninalink_msg.h"

#define NRFCLAW_NINALINK_STATE_CACHE_NODES            8U
#define NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE 24U
#define NRFCLAW_NINALINK_EVENT_HISTORY_DEPTH          8U

typedef struct {
    bool valid;
    uint16_t capability_id;
    uint8_t channel;
    nrfclaw_capability_value_t value;
    uint16_t last_sequence;
    uint8_t last_message_type;
    uint32_t updated_s;
    uint16_t update_count;
} nrfclaw_ninalink_cached_value_t;

typedef struct {
    bool valid;
    uint32_t node_id;
    uint32_t event_id;
    uint16_t capability_id;
    uint8_t channel;
    nrfclaw_capability_value_t value;
    uint16_t sequence;
    uint32_t occurred_s;
} nrfclaw_ninalink_cached_event_t;

typedef struct {
    bool valid;
    uint32_t node_id;
    uint8_t value_count;
    uint16_t updates;
    uint16_t reports;
    uint16_t events;
    uint16_t last_sequence;
    uint8_t last_message_type;
    uint32_t last_seen_s;
    bool session_valid;
    uint32_t session_id;
    uint16_t session_changes;
} nrfclaw_ninalink_state_cache_node_t;

typedef struct {
    uint8_t nodes;
    uint8_t values;
    uint16_t frames;
    uint16_t value_updates;
    uint16_t node_evictions;
    uint16_t value_evictions;
} nrfclaw_ninalink_state_cache_status_t;

typedef struct {
    uint8_t entries;
    uint16_t ingested;
    uint16_t dropped;
    uint32_t oldest_id;
    uint32_t newest_id;
} nrfclaw_ninalink_event_history_status_t;

void nrfclaw_ninalink_state_cache_clear(void);
bool nrfclaw_ninalink_state_cache_ingest_values(
    uint32_t node_id,
    uint16_t sequence,
    uint8_t message_type,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count,
    uint32_t now_s);
bool nrfclaw_ninalink_state_cache_ingest_values_session(
    uint32_t node_id,
    uint32_t session_id,
    uint16_t sequence,
    uint8_t message_type,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count,
    uint32_t now_s);
void nrfclaw_ninalink_state_cache_get_status(
    nrfclaw_ninalink_state_cache_status_t *out);
bool nrfclaw_ninalink_state_cache_get_node(
    uint32_t node_id,
    nrfclaw_ninalink_state_cache_node_t *out);
bool nrfclaw_ninalink_state_cache_get_value_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_cached_value_t *out);
void nrfclaw_ninalink_event_history_get_status(
    nrfclaw_ninalink_event_history_status_t *out);
uint8_t nrfclaw_ninalink_event_history_count(uint32_t node_id);
bool nrfclaw_ninalink_event_history_get_at(
    uint32_t node_id,
    uint8_t logical_index,
    nrfclaw_ninalink_cached_event_t *out);
bool nrfclaw_ninalink_event_history_get_global_at(
    uint8_t logical_index,
    nrfclaw_ninalink_cached_event_t *out);
bool nrfclaw_ninalink_event_history_get_by_id(
    uint32_t event_id,
    nrfclaw_ninalink_cached_event_t *out);
bool nrfclaw_ninalink_event_history_get_after(
    uint32_t cursor,
    nrfclaw_ninalink_cached_event_t *out);
void nrfclaw_ninalink_state_cache_get_revisions(
    uint32_t *state_revision,
    uint32_t *event_revision,
    uint32_t *change_revision);
uint32_t nrfclaw_ninalink_state_cache_value_raw(
    const nrfclaw_capability_value_t *value);

#endif
