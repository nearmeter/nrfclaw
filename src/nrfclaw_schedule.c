#include "nrfclaw_schedule.h"

void nrfclaw_schedule_encode(nrfclaw_schedule_t const *s,
                             uint8_t out[NRFCLAW_SCHEDULE_WIRE_SIZE])
{
    out[0] = s->mode;
    out[1] = s->dow_mask;
    out[2] = (uint8_t)(s->reserved & 0xffU);
    out[3] = (uint8_t)(s->reserved >> 8);

    out[4] = (uint8_t)(s->arg0 & 0xffU);
    out[5] = (uint8_t)((s->arg0 >> 8) & 0xffU);
    out[6] = (uint8_t)((s->arg0 >> 16) & 0xffU);
    out[7] = (uint8_t)((s->arg0 >> 24) & 0xffU);

    out[8]  = (uint8_t)(s->arg1 & 0xffU);
    out[9]  = (uint8_t)((s->arg1 >> 8) & 0xffU);
    out[10] = (uint8_t)((s->arg1 >> 16) & 0xffU);
    out[11] = (uint8_t)((s->arg1 >> 24) & 0xffU);
}

void nrfclaw_schedule_decode(nrfclaw_schedule_t *s,
                             uint8_t const in[NRFCLAW_SCHEDULE_WIRE_SIZE])
{
    s->mode = in[0];
    s->dow_mask = in[1];
    s->reserved = (uint16_t)in[2] | ((uint16_t)in[3] << 8);

    s->arg0 =
        ((uint32_t)in[4]) |
        ((uint32_t)in[5] << 8) |
        ((uint32_t)in[6] << 16) |
        ((uint32_t)in[7] << 24);

    s->arg1 =
        ((uint32_t)in[8]) |
        ((uint32_t)in[9] << 8) |
        ((uint32_t)in[10] << 16) |
        ((uint32_t)in[11] << 24);
}
