#ifndef NRFCLAW_NDP_ACCESS_H
#define NRFCLAW_NDP_ACCESS_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NRFCLAW_NDP_PUBLIC = 0,
    NRFCLAW_NDP_CONTROL = 1,
    NRFCLAW_NDP_PROVISION = 2
} nrfclaw_ndp_access_level_t;

void nrfclaw_ndp_access_init(void);
void nrfclaw_ndp_access_on_disconnect(void);
nrfclaw_ndp_access_level_t nrfclaw_ndp_access_level(void);
bool nrfclaw_ndp_access_allowed(nrfclaw_ndp_access_level_t required);

bool nrfclaw_ndp_access_begin(uint8_t requested_level,
                              uint8_t challenge[12],
                              uint8_t *session_id);
bool nrfclaw_ndp_access_finish(const uint8_t tag16[16]);
#endif
