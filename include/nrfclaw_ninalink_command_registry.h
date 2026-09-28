#ifndef NRFCLAW_NINALINK_COMMAND_REGISTRY_H
#define NRFCLAW_NINALINK_COMMAND_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink_command.h"

#define NRFCLAW_NINALINK_COMMAND_REGISTRY_RESULT_MAX 16U
#define NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY      0x01U

#define NRFCLAW_NINALINK_QUERY_MAX_ITEMS          2U
#define NRFCLAW_NINALINK_QUERY_RESULT_ENTRY_SIZE  6U

#define NRFCLAW_NINALINK_QUERY_ITEM_OK            0U
#define NRFCLAW_NINALINK_QUERY_ITEM_UNKNOWN       1U
#define NRFCLAW_NINALINK_QUERY_ITEM_NOT_READABLE  2U
#define NRFCLAW_NINALINK_QUERY_ITEM_UNAVAILABLE   3U

typedef uint8_t (*nrfclaw_ninalink_query_read_fn_t)(
    uint16_t capability_id,
    uint8_t channel,
    uint8_t *value_type,
    uint32_t *raw_value);

typedef struct {
    uint32_t node_id;
    bool tracking_active;
    nrfclaw_ninalink_query_read_fn_t query_read;
} nrfclaw_ninalink_command_context_t;

typedef struct {
    uint16_t command_id;
    uint8_t min_args;
    uint8_t max_args;
    uint8_t max_result;
    uint8_t flags;
} nrfclaw_ninalink_command_descriptor_t;

uint8_t nrfclaw_ninalink_command_registry_count(void);

bool nrfclaw_ninalink_command_registry_get(
    uint8_t index,
    nrfclaw_ninalink_command_descriptor_t *out);

uint8_t nrfclaw_ninalink_command_registry_execute(
    const nrfclaw_ninalink_command_context_t *context,
    uint16_t command_id,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len);

#endif
