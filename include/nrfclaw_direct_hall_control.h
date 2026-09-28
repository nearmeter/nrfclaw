#ifndef NRFCLAW_DIRECT_HALL_CONTROL_H
#define NRFCLAW_DIRECT_HALL_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_inputs.h"

/*
 * B7.6f2k3 persistent Direct Hall preset.
 *
 * Direct HA exposes behavior-level modes only.  Raw pull-up/count/event knobs
 * remain available to CLI/Studio/prompt through the legacy HALL_CONFIG path.
 */
void nrfclaw_direct_hall_control_init(void);
bool nrfclaw_direct_hall_control_apply_persisted(void);
bool nrfclaw_direct_hall_control_set_persist(nrfclaw_hall_mode_t mode,
                                             uint8_t channel);

nrfclaw_hall_mode_t nrfclaw_direct_hall_control_runtime_mode(void);
uint8_t nrfclaw_direct_hall_control_runtime_channel(void);
nrfclaw_hall_mode_t nrfclaw_direct_hall_control_persisted_mode(void);
uint8_t nrfclaw_direct_hall_control_persisted_channel(void);
bool nrfclaw_direct_hall_control_has_persisted(void);
uint8_t nrfclaw_direct_hall_control_store_status(void);

/* Reset the current count/position without changing the selected Hall mode. */
bool nrfclaw_direct_hall_control_reset_value(void);

#endif
