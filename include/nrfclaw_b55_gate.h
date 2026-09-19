#ifndef NRFCLAW_B55_GATE_H
#define NRFCLAW_B55_GATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_B55_GATE_IDLE = 0,
    NRFCLAW_B55_GATE_WAIT_STATE = 1,
    NRFCLAW_B55_GATE_STATE_RUNNING = 2,
    NRFCLAW_B55_GATE_WAIT_EVENT = 3,
    NRFCLAW_B55_GATE_EVENT_RUNNING = 4,
    NRFCLAW_B55_GATE_DONE = 5,
    NRFCLAW_B55_GATE_ERROR = 6
} nrfclaw_b55_gate_stage_t;

typedef enum {
    NRFCLAW_B55_GATE_ERR_NONE = 0,
    NRFCLAW_B55_GATE_ERR_TIMER_CREATE = 1,
    NRFCLAW_B55_GATE_ERR_TIMER_START = 2,
    NRFCLAW_B55_GATE_ERR_LINK_BUSY = 3,
    NRFCLAW_B55_GATE_ERR_STATE_START = 4,
    NRFCLAW_B55_GATE_ERR_STATE_RESULT = 5,
    NRFCLAW_B55_GATE_ERR_EVENT_START = 6,
    NRFCLAW_B55_GATE_ERR_EVENT_RESULT = 7
} nrfclaw_b55_gate_error_t;

typedef struct {
    uint8_t stage;
    uint8_t error;
    uint8_t state_result;
    uint8_t event_result;
    bool state_sent;
    bool event_sent;
    bool armed;
} nrfclaw_b55_gate_status_t;

bool nrfclaw_b55_gate_arm(uint8_t state_delay_s, uint8_t event_gap_s);
void nrfclaw_b55_gate_process(void);
void nrfclaw_b55_gate_get_status(nrfclaw_b55_gate_status_t *out);

#endif
