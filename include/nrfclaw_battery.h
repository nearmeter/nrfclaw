#ifndef NRFCLAW_BATTERY_H
#define NRFCLAW_BATTERY_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"

/* Start a non-blocking battery acquisition. Completion is delivered as
 * NRFCLAW_EVT_BATTERY_DONE with arg0 in centivolts (e.g. 365 = 3.65 V). */
void nrfclaw_battery_init(void);
bool nrfclaw_battery_start(void);
bool nrfclaw_battery_busy(void);
bool nrfclaw_battery_last(uint16_t *centivolts);
void nrfclaw_battery_on_event(nrfclaw_event_t const *evt);

#endif
