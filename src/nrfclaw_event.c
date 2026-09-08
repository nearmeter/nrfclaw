#include "nrfclaw_event.h"
#include "app_util_platform.h"

#define QSZ 16
static volatile uint8_t q_head, q_tail;
static nrfclaw_event_t q[QSZ];

void nrfclaw_event_init(void) { q_head = q_tail = 0; }

static bool push(nrfclaw_event_t const *evt)
{
    uint8_t next = (uint8_t)((q_head + 1U) % QSZ);
    if (next == q_tail) return false;
    q[q_head] = *evt;
    q_head = next;
    return true;
}

bool nrfclaw_event_push_isr(nrfclaw_event_t const *evt) { return push(evt); }

bool nrfclaw_event_push(nrfclaw_event_t const *evt)
{
    bool ok;
    CRITICAL_REGION_ENTER();
    ok = push(evt);
    CRITICAL_REGION_EXIT();
    return ok;
}

bool nrfclaw_event_pop(nrfclaw_event_t *evt)
{
    bool ok = false;
    CRITICAL_REGION_ENTER();
    if (q_tail != q_head) {
        *evt = q[q_tail];
        q_tail = (uint8_t)((q_tail + 1U) % QSZ);
        ok = true;
    }
    CRITICAL_REGION_EXIT();
    return ok;
}
