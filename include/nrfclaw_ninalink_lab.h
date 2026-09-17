#ifndef NRFCLAW_NINALINK_LAB_H
#define NRFCLAW_NINALINK_LAB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_NINALINK_LAB_IDLE = 0,
    NRFCLAW_NINALINK_LAB_SENT = 1,
    NRFCLAW_NINALINK_LAB_NO_DATA = 2,
    NRFCLAW_NINALINK_LAB_RADIO_BUSY = 3,
    NRFCLAW_NINALINK_LAB_BUILD_ERROR = 4,
    NRFCLAW_NINALINK_LAB_TIMER_ERROR = 5
} nrfclaw_ninalink_lab_result_t;

typedef struct {
    bool active;
    uint16_t period_s;
    uint16_t next_sequence;
    uint8_t last_entry_count;
    uint8_t last_frame_length;
    uint8_t last_result;
    uint32_t node_id;
} nrfclaw_ninalink_lab_status_t;

bool nrfclaw_ninalink_lab_init(void);
bool nrfclaw_ninalink_lab_start(uint16_t period_s);
void nrfclaw_ninalink_lab_stop(void);
void nrfclaw_ninalink_lab_process(void);
void nrfclaw_ninalink_lab_get_status(nrfclaw_ninalink_lab_status_t *out);

#endif
