#ifndef NRFCLAW_NINALINK_QUERY_H
#define NRFCLAW_NINALINK_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_NINALINK_QUERY_ENDPOINT_MAX 2U

typedef enum {
    NRFCLAW_NINALINK_QUERY_IDLE = 0,
    NRFCLAW_NINALINK_QUERY_PENDING = 1,
    NRFCLAW_NINALINK_QUERY_DONE = 2,
    NRFCLAW_NINALINK_QUERY_ERROR = 3
} nrfclaw_ninalink_query_state_t;

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
} nrfclaw_ninalink_query_endpoint_t;

typedef struct {
    uint8_t state;
    uint8_t count;
    uint32_t target_node;
    uint16_t command_seq;
    uint8_t command_result;
    uint8_t cached_count;
    uint8_t item_status[NRFCLAW_NINALINK_QUERY_ENDPOINT_MAX];
} nrfclaw_ninalink_query_status_t;

void nrfclaw_ninalink_query_reset(void);
bool nrfclaw_ninalink_query_queue(
    uint32_t target_node,
    const nrfclaw_ninalink_query_endpoint_t *endpoints,
    uint8_t count);
void nrfclaw_ninalink_query_get_status(nrfclaw_ninalink_query_status_t *out);
void nrfclaw_ninalink_query_on_command_result(
    uint32_t target_node,
    uint16_t command_seq,
    uint16_t command_id,
    uint8_t command_result,
    const uint8_t *result_data,
    uint8_t result_len,
    uint32_t now_s);

#endif
