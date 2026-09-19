#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrfclaw_ninalink_state_cache.h"

static unsigned g_tests;
static unsigned g_failures;
#define CHECK(name, expr) do { \
    g_tests++; \
    if (expr) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); g_failures++; } \
} while (0)

static void set_temp(nrfclaw_ninalink_value_entry_t *e, int16_t v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = 0x0100U;
    e->channel = 0U;
    e->value.type = NRFCLAW_CAP_VALUE_S16;
    e->value.v.s16 = v;
}

int main(void)
{
    const uint32_t NODE = 0xAD64D423UL;
    const uint32_t A = 0x11111111UL;
    const uint32_t B = 0x22222222UL;
    nrfclaw_ninalink_value_entry_t e;
    nrfclaw_ninalink_cached_value_t v;
    nrfclaw_ninalink_state_cache_node_t n;

    nrfclaw_ninalink_state_cache_clear();

    set_temp(&e, 1975);
    CHECK("session A seq1",
        nrfclaw_ninalink_state_cache_ingest_values_session(
            NODE,A,1U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,10U));

    set_temp(&e, 2025);
    CHECK("session A seq3",
        nrfclaw_ninalink_state_cache_ingest_values_session(
            NODE,A,3U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,20U));

    CHECK("session A state",
        nrfclaw_ninalink_state_cache_get_value_at(NODE,0U,&v) &&
        v.value.v.s16==2025 && v.last_sequence==3U && v.update_count==2U);

    set_temp(&e, 2100);
    CHECK("session B low seq1 accepted",
        nrfclaw_ninalink_state_cache_ingest_values_session(
            NODE,B,1U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,30U));

    CHECK("session B starts clean value epoch",
        nrfclaw_ninalink_state_cache_get_value_at(NODE,0U,&v) &&
        v.value.v.s16==2100 && v.last_sequence==1U && v.update_count==1U);

    CHECK("session transition summary",
        nrfclaw_ninalink_state_cache_get_node(NODE,&n) &&
        n.session_valid && n.session_id==B && n.session_changes==1U &&
        n.last_sequence==1U && n.value_count==1U);

    set_temp(&e, 1500);
    CHECK("session B stale seq0 consumed",
        nrfclaw_ninalink_state_cache_ingest_values_session(
            NODE,B,0U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,40U));

    CHECK("session B stale cannot regress",
        nrfclaw_ninalink_state_cache_get_value_at(NODE,0U,&v) &&
        v.value.v.s16==2100 && v.last_sequence==1U && v.update_count==1U);

    CHECK("frame counters remain cumulative",
        nrfclaw_ninalink_state_cache_get_node(NODE,&n) &&
        n.updates==4U && n.reports==4U && n.events==0U &&
        n.last_sequence==1U && n.session_changes==1U);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
