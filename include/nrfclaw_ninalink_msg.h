#ifndef NRFCLAW_NINALINK_MSG_H
#define NRFCLAW_NINALINK_MSG_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_capability.h"
#include "nrfclaw_ninalink.h"

#define NRFCLAW_NINALINK_HELLO_PAYLOAD_SIZE          19U
#define NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE        4U
#define NRFCLAW_NINALINK_CAPS_DESCRIPTOR_SIZE         9U
#define NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS         5U
#define NRFCLAW_NINALINK_ACK_PAYLOAD_SIZE             3U
#define NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE      4U
#define NRFCLAW_NINALINK_MAX_VALUE_ENTRIES           12U

typedef enum {
    NRFCLAW_NINALINK_MSG_OK = 0,
    NRFCLAW_NINALINK_MSG_ERR_BAD_ARG,
    NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE,
    NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH,
    NRFCLAW_NINALINK_MSG_ERR_TOO_MANY,
    NRFCLAW_NINALINK_MSG_ERR_NO_SPACE,
    NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE,
    NRFCLAW_NINALINK_MSG_ERR_UNSUPPORTED_VALUE_TYPE
} nrfclaw_ninalink_msg_status_t;

typedef enum {
    NRFCLAW_NINALINK_NACK_BAD_FRAME    = 0x01,
    NRFCLAW_NINALINK_NACK_UNSUPPORTED  = 0x02,
    NRFCLAW_NINALINK_NACK_BUSY         = 0x03,
    NRFCLAW_NINALINK_NACK_UNAUTHORIZED = 0x04,
    NRFCLAW_NINALINK_NACK_BAD_VALUE    = 0x05,
    NRFCLAW_NINALINK_NACK_INTERNAL     = 0x06
} nrfclaw_ninalink_nack_reason_t;

typedef struct {
    uint32_t device_id0;
    uint32_t device_id1;
    uint8_t capability_registry_version;
    uint8_t advertised_capability_count;
    uint8_t board_type;
    uint8_t hw_rev_major;
    uint8_t hw_rev_minor;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    uint8_t fw_prerelease;
    uint16_t fw_build;
} nrfclaw_ninalink_hello_t;

typedef struct {
    nrfclaw_capability_desc_t desc;
    uint8_t runtime_state_flags;
} nrfclaw_ninalink_capability_descriptor_t;

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
    nrfclaw_capability_value_t value;
} nrfclaw_ninalink_value_entry_t;

/* HELLO */
nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_hello(nrfclaw_ninalink_frame_t *frame,
                             uint16_t network_id,
                             uint32_t node_id,
                             uint16_t sequence,
                             bool ack_req,
                             const nrfclaw_ninalink_hello_t *hello);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_hello(const nrfclaw_ninalink_frame_t *frame,
                             nrfclaw_ninalink_hello_t *hello);

/* CAPS_REQUEST */
nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_caps_request(nrfclaw_ninalink_frame_t *frame,
                                    uint16_t network_id,
                                    uint32_t node_id,
                                    uint16_t sequence,
                                    bool ack_req,
                                    uint8_t page_index);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_caps_request(const nrfclaw_ninalink_frame_t *frame,
                                    uint8_t *page_index);

/* CAPS_RESPONSE */
nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_caps_response(
    nrfclaw_ninalink_frame_t *frame,
    uint16_t network_id,
    uint32_t node_id,
    uint16_t sequence,
    bool ack_req,
    bool more,
    uint8_t registry_version,
    uint8_t page_index,
    const nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t descriptor_count);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_caps_response(
    const nrfclaw_ninalink_frame_t *frame,
    uint8_t *registry_version,
    uint8_t *page_index,
    nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t descriptor_capacity,
    uint8_t *descriptor_count,
    bool *more);

/* CAP_REPORT / CAP_EVENT */
nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_values(
    nrfclaw_ninalink_frame_t *frame,
    uint8_t message_type,
    uint16_t network_id,
    uint32_t node_id,
    uint16_t sequence,
    bool ack_req,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_values(
    const nrfclaw_ninalink_frame_t *frame,
    nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_capacity,
    uint8_t *entry_count);

/* ACK / NACK */
nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_ack(nrfclaw_ninalink_frame_t *frame,
                           uint16_t network_id,
                           uint32_t node_id,
                           uint16_t sequence,
                           uint16_t acknowledged_sequence);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_ack(const nrfclaw_ninalink_frame_t *frame,
                           uint16_t *acknowledged_sequence);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_nack(nrfclaw_ninalink_frame_t *frame,
                            uint16_t network_id,
                            uint32_t node_id,
                            uint16_t sequence,
                            uint16_t rejected_sequence,
                            uint8_t reason);

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_nack(const nrfclaw_ninalink_frame_t *frame,
                            uint16_t *rejected_sequence,
                            uint8_t *reason);

const char *
nrfclaw_ninalink_msg_status_str(nrfclaw_ninalink_msg_status_t status);

#endif
