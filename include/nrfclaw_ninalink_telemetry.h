#ifndef NRFCLAW_NINALINK_TELEMETRY_H
#define NRFCLAW_NINALINK_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_event.h"

typedef enum {
    NRFCLAW_NINALINK_TELEM_IDLE = 0,
    NRFCLAW_NINALINK_TELEM_WAIT_TEMP = 1,
    NRFCLAW_NINALINK_TELEM_WAIT_LINK = 2
} nrfclaw_ninalink_telemetry_stage_t;

typedef struct {
    bool enabled;
    uint8_t stage;
    uint16_t period_s;
    uint16_t ack_window_ms;
    uint8_t max_attempts;
    uint16_t base_backoff_ms;

    uint32_t ticks;
    uint32_t cycles_started;
    uint32_t cycles_completed;
    uint32_t acked;
    uint32_t timed_out;
    uint32_t link_errors;
    uint32_t sensor_errors;
    uint32_t overruns;

    int32_t last_temperature_mC;
    uint8_t last_link_result;
} nrfclaw_ninalink_telemetry_status_t;

void nrfclaw_ninalink_telemetry_init(void);

bool nrfclaw_ninalink_telemetry_start(
    uint16_t period_s,
    uint16_t ack_window_ms,
    uint8_t max_attempts,
    uint16_t base_backoff_ms);

bool nrfclaw_ninalink_telemetry_start_synthetic(
    uint16_t period_s,
    uint16_t ack_window_ms,
    uint8_t max_attempts,
    uint16_t base_backoff_ms,
    int32_t temperature_mC);

void nrfclaw_ninalink_telemetry_stop(void);
void nrfclaw_ninalink_telemetry_process(void);
void nrfclaw_ninalink_telemetry_on_event(const nrfclaw_event_t *evt);
void nrfclaw_ninalink_telemetry_get_status(
    nrfclaw_ninalink_telemetry_status_t *out);

#endif
