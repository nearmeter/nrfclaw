#ifndef NRFCLAW_NINALINK_COMMAND_DISCOVERY_H
#define NRFCLAW_NINALINK_COMMAND_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_ninalink_command_registry.h"

#define NRFCLAW_NINALINK_MSG_COMMANDS_REQUEST   0x33U
#define NRFCLAW_NINALINK_MSG_COMMANDS_RESPONSE  0x34U

#define NRFCLAW_NINALINK_COMMAND_REGISTRY_VERSION 1U
#define NRFCLAW_NINALINK_COMMAND_DESCRIPTOR_WIRE_SIZE 6U
#define NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX 7U

typedef struct {
    uint8_t registry_version;
    uint8_t start_index;
    uint8_t total_count;
    uint8_t count;
    nrfclaw_ninalink_command_descriptor_t
        descriptors[NRFCLAW_NINALINK_COMMAND_DISCOVERY_PAGE_MAX];
} nrfclaw_ninalink_command_discovery_page_t;

bool nrfclaw_ninalink_command_discovery_build_page(
    uint8_t start_index,
    uint8_t *payload,
    uint8_t payload_capacity,
    uint8_t *payload_len);

bool nrfclaw_ninalink_command_discovery_parse_page(
    const uint8_t *payload,
    uint8_t payload_len,
    nrfclaw_ninalink_command_discovery_page_t *out);

#endif
