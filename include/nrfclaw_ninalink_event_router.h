#ifndef NRFCLAW_NINALINK_EVENT_ROUTER_H
#define NRFCLAW_NINALINK_EVENT_ROUTER_H

#include <stdint.h>

#include "nrfclaw_event.h"

#define NRFCLAW_NINALINK_EVENT_ROUTER_DEPTH 4U

typedef struct {
    uint8_t queued;
    uint16_t enqueued;
    uint16_t sent;
    uint16_t dropped;
    uint16_t ignored;
    uint16_t last_capability_id;
} nrfclaw_ninalink_event_router_status_t;

void nrfclaw_ninalink_event_router_on_event(
    const nrfclaw_event_t *event);

void nrfclaw_ninalink_event_router_process(void);
void nrfclaw_ninalink_event_router_reset(void);

void nrfclaw_ninalink_event_router_get_status(
    nrfclaw_ninalink_event_router_status_t *out);

#endif
