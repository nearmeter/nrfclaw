#ifndef NRFCLAW_NINALINK_COMMAND_REGISTRY_H
#define NRFCLAW_NINALINK_COMMAND_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink_command.h"

#define NRFCLAW_NINALINK_COMMAND_REGISTRY_RESULT_MAX 16U
#define NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY      0x01U

typedef struct {
    uint32_t node_id;
    bool tracking_active;
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
