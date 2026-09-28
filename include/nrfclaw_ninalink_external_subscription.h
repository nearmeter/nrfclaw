#ifndef NRFCLAW_NINALINK_EXTERNAL_SUBSCRIPTION_H
#define NRFCLAW_NINALINK_EXTERNAL_SUBSCRIPTION_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_NINALINK_CHANGE_SCHEMA_VERSION 1U
#define NRFCLAW_NINALINK_CHANGE_MAGIC          0xE5U
#define NRFCLAW_NINALINK_CHANGE_FRAME_SIZE     20U

#define NRFCLAW_NINALINK_CHANGE_STATE 0x01U
#define NRFCLAW_NINALINK_CHANGE_EVENT 0x02U
#define NRFCLAW_NINALINK_CHANGE_ALL   0x03U

typedef struct {
    uint8_t schema_version;
    uint8_t mask;
    uint8_t pending_flags;
    uint32_t state_revision;
    uint32_t event_revision;
    uint32_t newest_event_id;
} nrfclaw_ninalink_change_status_t;

typedef struct {
    uint32_t change_revision;
    uint16_t notifications_sent;
    uint16_t coalesced;
    uint16_t busy_retries;
    uint16_t disconnect_resets;
    uint16_t send_errors;
} nrfclaw_ninalink_change_stats_t;

void nrfclaw_ninalink_external_subscription_set(uint8_t mask);
void nrfclaw_ninalink_external_subscription_reset(void);
void nrfclaw_ninalink_external_subscription_process(void);
void nrfclaw_ninalink_external_subscription_get_status(
    nrfclaw_ninalink_change_status_t *out);
void nrfclaw_ninalink_external_subscription_get_stats(
    nrfclaw_ninalink_change_stats_t *out);

#endif
