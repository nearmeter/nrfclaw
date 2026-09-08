#ifndef NRFCLAW_STATE_H
#define NRFCLAW_STATE_H
#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_STATE_USER_COUNT 16U
#define NRFCLAW_STATE_COUNT 17U /* includes internal r3.8.10 boot-control key */
#define NRFCLAW_STATE_PAGE0_ADDR 0x00072000UL
#define NRFCLAW_STATE_PAGE1_ADDR 0x00073000UL

typedef enum {
    NRFCLAW_STATE_IDLE = 0,
    NRFCLAW_STATE_SAVING,
    NRFCLAW_STATE_ERROR
} nrfclaw_state_status_t;

void nrfclaw_state_init(void);
void nrfclaw_state_process(void);

bool nrfclaw_state_set(uint8_t key, uint32_t value);
bool nrfclaw_state_get(uint8_t key, uint32_t *value);
bool nrfclaw_state_has(uint8_t key);

/* Async persistent save. Value is copied immediately. */
bool nrfclaw_state_persist(uint8_t key, uint32_t value);

nrfclaw_state_status_t nrfclaw_state_status(void);
uint32_t nrfclaw_state_sequence(void);

#endif
