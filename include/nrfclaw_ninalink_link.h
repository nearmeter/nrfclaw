#ifndef NRFCLAW_NINALINK_LINK_H
#define NRFCLAW_NINALINK_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_NINALINK_LINK_IDLE = 0,
    NRFCLAW_NINALINK_LINK_WAIT_TX = 1,
    NRFCLAW_NINALINK_LINK_WAIT_ACK = 2,
    NRFCLAW_NINALINK_LINK_BACKOFF = 3,
    NRFCLAW_NINALINK_LINK_DONE = 4,
    NRFCLAW_NINALINK_LINK_APP_RESULT_ARM = 5,
    NRFCLAW_NINALINK_LINK_APP_RESULT_TX = 6
} nrfclaw_ninalink_link_state_t;

typedef enum {
    NRFCLAW_NINALINK_LINK_RESULT_NONE = 0,
    NRFCLAW_NINALINK_LINK_RESULT_ACKED = 1,
    NRFCLAW_NINALINK_LINK_RESULT_TIMEOUT = 2,
    NRFCLAW_NINALINK_LINK_RESULT_BAD_ACK = 3,
    NRFCLAW_NINALINK_LINK_RESULT_TX_FAIL = 4,
    NRFCLAW_NINALINK_LINK_RESULT_RX_FAIL = 5,
    NRFCLAW_NINALINK_LINK_RESULT_NO_DATA = 6,
    NRFCLAW_NINALINK_LINK_RESULT_BUILD_FAIL = 7,
    NRFCLAW_NINALINK_LINK_RESULT_RETRY_TIMER_FAIL = 8
} nrfclaw_ninalink_link_result_t;

typedef enum {
    NRFCLAW_NINALINK_APP_OK = 0,
    NRFCLAW_NINALINK_APP_UNSUPPORTED_CAP = 1,
    NRFCLAW_NINALINK_APP_BAD_TYPE = 2,
    NRFCLAW_NINALINK_APP_BAD_VALUE = 3,
    NRFCLAW_NINALINK_APP_APPLY_FAILED = 4
} nrfclaw_ninalink_app_result_t;

typedef struct {
    uint8_t state;
    uint8_t result;
    uint16_t sequence;
    uint8_t tx_len;
    uint8_t ack_len;
    int16_t ack_rssi_x2;
    int16_t ack_snr_x4;
    uint16_t acked_count;
    uint16_t timeout_count;
    uint8_t attempts;
    uint8_t max_attempts;
    uint16_t retry_count;
    uint16_t base_backoff_ms;
    uint16_t last_backoff_ms;
} nrfclaw_ninalink_link_status_t;

typedef struct {
    bool valid;
    uint16_t sequence;
    uint8_t result;
    uint16_t applied_count;
    uint16_t duplicate_count;
    bool tracking_active;
} nrfclaw_ninalink_app_status_t;

bool nrfclaw_ninalink_link_start(uint16_t ack_window_ms);
bool nrfclaw_ninalink_link_start_reliable(uint16_t ack_window_ms,
                                          uint8_t max_attempts,
                                          uint16_t base_backoff_ms);
void nrfclaw_ninalink_link_process(void);
void nrfclaw_ninalink_link_get_status(nrfclaw_ninalink_link_status_t *out);
void nrfclaw_ninalink_link_get_app_status(nrfclaw_ninalink_app_status_t *out);

#endif
