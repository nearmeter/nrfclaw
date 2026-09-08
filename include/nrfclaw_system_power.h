#ifndef NRFCLAW_SYSTEM_POWER_H
#define NRFCLAW_SYSTEM_POWER_H

/* Prepare the board for baseline current measurement.
 * P0.21 programming/wake remains active; this function never touches it. */
void nrfclaw_system_minimum_power(void);

#endif
