#ifndef NRFCLAW_SCHEDULE_H
#define NRFCLAW_SCHEDULE_H

#include <stdint.h>

typedef enum
{
    NRFCLAW_SCHEDULE_MANUAL = 0,
    NRFCLAW_SCHEDULE_AT     = 1,
    NRFCLAW_SCHEDULE_EVERY  = 2,
    NRFCLAW_SCHEDULE_WEEKLY = 3,
    NRFCLAW_SCHEDULE_BOOT   = 4
} nrfclaw_schedule_mode_t;

typedef struct
{
    uint8_t  mode;
    uint8_t  dow_mask;
    uint16_t reserved;
    uint32_t arg0;
    uint32_t arg1;
} nrfclaw_schedule_t;

#define NRFCLAW_SCHEDULE_WIRE_SIZE 12U

void nrfclaw_schedule_encode(nrfclaw_schedule_t const *s,
                             uint8_t out[NRFCLAW_SCHEDULE_WIRE_SIZE]);

void nrfclaw_schedule_decode(nrfclaw_schedule_t *s,
                             uint8_t const in[NRFCLAW_SCHEDULE_WIRE_SIZE]);

#endif
