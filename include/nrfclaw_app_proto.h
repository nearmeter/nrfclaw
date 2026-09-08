#ifndef NRFCLAW_APP_PROTO_H
#define NRFCLAW_APP_PROTO_H

#include <stdint.h>

#define NRFCLAW_APP_PROTO_VERSION 1U
#define NRFCLAW_APP_MAX_PAYLOAD   48U

/* Common operations used by bridge firmware. */
#define NRFCLAW_APP_OP_READ        1U
#define NRFCLAW_APP_OP_WRITE       2U
#define NRFCLAW_APP_OP_SUBSCRIBE   3U
#define NRFCLAW_APP_OP_TRIGGER     4U

typedef enum {
    NRFCLAW_APP_MSG_PING = 1,
    NRFCLAW_APP_MSG_GET = 2,
    NRFCLAW_APP_MSG_SET = 3,
    NRFCLAW_APP_MSG_EVENT = 4,
    NRFCLAW_APP_MSG_REPLY = 5,
    NRFCLAW_APP_MSG_ERROR = 6,
    NRFCLAW_APP_MSG_COMMAND = 7
} nrfclaw_app_msg_type_t;

typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t capability;
    uint8_t operation;
    uint8_t length;
    uint8_t payload[NRFCLAW_APP_MAX_PAYLOAD];
} nrfclaw_app_frame_t;

#endif
