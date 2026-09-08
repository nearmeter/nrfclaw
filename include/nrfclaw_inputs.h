#ifndef NRFCLAW_INPUTS_H
#define NRFCLAW_INPUTS_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_HALL_MODE_DISABLED=0,
    NRFCLAW_HALL_MODE_SINGLE=1,      /* selected HALL1 or HALL2 */
    NRFCLAW_HALL_MODE_QUADRATURE=2   /* HALL1 + HALL2 */
} nrfclaw_hall_mode_t;

typedef struct {
    nrfclaw_hall_mode_t mode;
    uint8_t channel; /* 1=HALL1, 2=HALL2; ignored in quadrature */
    bool pullup;
    bool count_edges;
    bool emit_events;
} nrfclaw_hall_config_t;

void nrfclaw_inputs_init(void);
bool nrfclaw_hall_configure(nrfclaw_hall_config_t const *cfg);
void nrfclaw_hall_disable(void);
bool nrfclaw_hall_active(void);
nrfclaw_hall_mode_t nrfclaw_hall_mode(void);
int32_t nrfclaw_hall_position(void);
uint32_t nrfclaw_hall_count(void);
uint8_t nrfclaw_hall_channel(void);
void nrfclaw_hall_diag(uint8_t *hall1_pin,uint8_t *hall2_pin,uint32_t *hall1_rc,uint32_t *hall2_rc);

/* Backward-compatible helper: enables quadrature. */
bool nrfclaw_hall_enable(bool pullup);
#endif
