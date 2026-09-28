#ifndef NRFCLAW_HA_NINALINK_NODE_H
#define NRFCLAW_HA_NINALINK_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"

#define NRFCLAW_HA_NODE_DEFAULT_PERIOD_S  20U
#define NRFCLAW_HA_NODE_MIN_PERIOD_S       5U
#define NRFCLAW_HA_NODE_MAX_PERIOD_S    3600U
#define NRFCLAW_HA_NODE_ACK_WINDOW_MS    600U
#define NRFCLAW_HA_NODE_ATTEMPTS        3U
#define NRFCLAW_HA_NODE_BACKOFF_MS    200U

typedef struct {
    bool enabled;
    uint8_t stage;
    uint16_t period_s;
    uint32_t reports_started;
    uint32_t reports_completed;
    uint32_t acked;
    uint32_t timed_out;
    uint32_t sensor_errors;
    uint8_t last_link_result;
} nrfclaw_ha_ninalink_node_status_t;

bool nrfclaw_ha_ninalink_node_start(uint16_t period_s);
void nrfclaw_ha_ninalink_node_stop(void);
void nrfclaw_ha_ninalink_node_process(void);
void nrfclaw_ha_ninalink_node_on_event(const nrfclaw_event_t *evt);
void nrfclaw_ha_ninalink_node_get_status(nrfclaw_ha_ninalink_node_status_t *out);

#endif
