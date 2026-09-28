#ifndef NRFCLAW_NINALINK_AUTO_DISCOVERY_H
#define NRFCLAW_NINALINK_AUTO_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_NINALINK_AUTO_KIND_NONE = 0,
    NRFCLAW_NINALINK_AUTO_KIND_CAPABILITY = 1,
    NRFCLAW_NINALINK_AUTO_KIND_COMMAND = 2
} nrfclaw_ninalink_auto_kind_t;

typedef struct {
    bool enabled;
    bool inflight;
    uint8_t kind;
    uint32_t node_id;
    uint16_t request_seq;
} nrfclaw_ninalink_auto_discovery_status_t;

void nrfclaw_ninalink_auto_discovery_reset_runtime(void);
void nrfclaw_ninalink_auto_discovery_on_contact(uint32_t node_id);
void nrfclaw_ninalink_auto_discovery_process(void);
void nrfclaw_ninalink_auto_discovery_set_enabled(bool enabled);
void nrfclaw_ninalink_auto_discovery_get_status(
    nrfclaw_ninalink_auto_discovery_status_t *out);
void nrfclaw_ninalink_auto_discovery_abort(void);

#endif
