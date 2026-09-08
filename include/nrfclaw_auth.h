#ifndef NRFCLAW_AUTH_H
#define NRFCLAW_AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_schedule.h"

#define NRFCLAW_AUTH_TAG_SIZE       32U
#define NRFCLAW_AUTH_KEY_SIZE       32U

/*
 * HMAC domain separation string:
 *
 *   "nRFClaw-bytecode-v5\0"
 *
 * Authenticated message:
 *
 *   domain || len:u16-le || bytecode[len]
 */
void nrfclaw_auth_compute(uint8_t const *program,
                          uint16_t len,
                          nrfclaw_schedule_t const *schedule,
                          uint8_t tag[NRFCLAW_AUTH_TAG_SIZE]);

bool nrfclaw_auth_verify(uint8_t const *program,
                         uint16_t len,
                         nrfclaw_schedule_t const *schedule,
                         uint8_t const tag[NRFCLAW_AUTH_TAG_SIZE]);

#endif
