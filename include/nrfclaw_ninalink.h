#ifndef NRFCLAW_NINALINK_H
#define NRFCLAW_NINALINK_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_NINALINK_MAGIC            0x4EU
#define NRFCLAW_NINALINK_VERSION          0x01U

#define NRFCLAW_NINALINK_HEADER_SIZE      13U
#define NRFCLAW_NINALINK_CRC_SIZE          2U
#define NRFCLAW_NINALINK_MAX_FRAME_SIZE   64U
#define NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE 49U
#define NRFCLAW_NINALINK_MIN_FRAME_SIZE   15U

#define NRFCLAW_NINALINK_FLAG_ACK_REQ     0x01U
#define NRFCLAW_NINALINK_FLAG_MORE        0x02U
#define NRFCLAW_NINALINK_FLAG_AUTH        0x04U
#define NRFCLAW_NINALINK_FLAG_ENCRYPTED   0x08U

/* AUTH and ENCRYPTED are reserved by B1.2 but are not implemented by v1/B3.1.
 * B3.1 therefore accepts only ACK_REQ and MORE. */
#define NRFCLAW_NINALINK_V1_FLAGS_MASK \
    (NRFCLAW_NINALINK_FLAG_ACK_REQ | NRFCLAW_NINALINK_FLAG_MORE)

#define NRFCLAW_NINALINK_NODE_BROADCAST   0xFFFFFFFFUL

typedef enum {
    NRFCLAW_NINALINK_MSG_HELLO         = 0x01,
    NRFCLAW_NINALINK_MSG_CAPS_REQUEST  = 0x02,
    NRFCLAW_NINALINK_MSG_CAPS_RESPONSE = 0x03,

    NRFCLAW_NINALINK_MSG_CAP_REPORT    = 0x10,
    NRFCLAW_NINALINK_MSG_CAP_EVENT     = 0x11,

    NRFCLAW_NINALINK_MSG_ACK           = 0x20,
    NRFCLAW_NINALINK_MSG_NACK          = 0x21,

    NRFCLAW_NINALINK_MSG_CAP_READ      = 0x30,
    NRFCLAW_NINALINK_MSG_CAP_SET       = 0x31,
    NRFCLAW_NINALINK_MSG_COMMAND       = 0x32
} nrfclaw_ninalink_message_type_t;

typedef enum {
    NRFCLAW_NINALINK_OK = 0,
    NRFCLAW_NINALINK_ERR_BAD_ARG,
    NRFCLAW_NINALINK_ERR_NO_SPACE,
    NRFCLAW_NINALINK_ERR_TOO_SHORT,
    NRFCLAW_NINALINK_ERR_TOO_LONG,
    NRFCLAW_NINALINK_ERR_BAD_MAGIC,
    NRFCLAW_NINALINK_ERR_BAD_VERSION,
    NRFCLAW_NINALINK_ERR_BAD_FLAGS,
    NRFCLAW_NINALINK_ERR_BAD_LENGTH,
    NRFCLAW_NINALINK_ERR_BAD_CRC
} nrfclaw_ninalink_status_t;

typedef struct {
    uint8_t flags;
    uint8_t message_type;
    uint16_t network_id;
    uint32_t node_id;
    uint16_t sequence;
    uint8_t payload_length;
    uint8_t payload[NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE];
} nrfclaw_ninalink_frame_t;

/* CRC-16/CCITT-FALSE:
 * poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0x0000. */
uint16_t nrfclaw_ninalink_crc16(const uint8_t *data, uint16_t length);

/* Encode one complete NinaLink v1 frame, including CRC.
 * On success, *out_length is in the range 15..64. */
nrfclaw_ninalink_status_t
nrfclaw_ninalink_encode(const nrfclaw_ninalink_frame_t *frame,
                        uint8_t *out,
                        uint8_t out_capacity,
                        uint8_t *out_length);

/* Decode and validate one complete NinaLink v1 frame.
 * Unknown message_type values are intentionally accepted; semantic handling
 * belongs to higher layers. */
nrfclaw_ninalink_status_t
nrfclaw_ninalink_decode(const uint8_t *data,
                        uint8_t length,
                        nrfclaw_ninalink_frame_t *out);

/* Stable diagnostic string; never returns NULL. */
const char *nrfclaw_ninalink_status_str(nrfclaw_ninalink_status_t status);

#endif
