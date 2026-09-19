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

static void set_s16(nrfclaw_ninalink_value_entry_t *e,
                    uint16_t id, uint8_t channel, int16_t value)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = channel;
    e->value.type = NRFCLAW_CAP_VALUE_S16;
    e->value.v.s16 = value;
}

int main(void)
{
    const uint32_t A = 0xAD64D423UL;
    nrfclaw_ninalink_value_entry_t e;
    nrfclaw_ninalink_cached_value_t value;
    nrfclaw_ninalink_state_cache_node_t node;

    nrfclaw_ninalink_state_cache_clear();

    set_s16(&e, 0x0100U, 0U, 1975);
    CHECK("initial seq1000",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,1000U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,10U));

    set_s16(&e, 0x0100U, 0U, 2025);
    CHECK("newer seq1002",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,1002U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,20U));

    set_s16(&e, 0x0100U, 0U, 1500);
    CHECK("stale seq1001 frame accepted",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,1001U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,30U));

    CHECK("stale cannot overwrite",
          nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) &&
          value.value.type==NRFCLAW_CAP_VALUE_S16 &&
          value.value.v.s16==2025 &&
          value.last_sequence==1002U &&
          value.update_count==2U);

    CHECK("node frame counters include stale arrival",
          nrfclaw_ninalink_state_cache_get_node(A,&node) &&
          node.updates==3U &&
          node.reports==3U &&
          node.last_sequence==1002U);

    set_s16(&e, 0x0100U, 0U, 999);
    CHECK("equal seq frame accepted",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,1002U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,40U));

    CHECK("equal seq cannot overwrite",
          nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) &&
          value.value.v.s16==2025 &&
          value.last_sequence==1002U &&
          value.update_count==2U);

    set_s16(&e, 0x9001U, 0U, 77);
    CHECK("older frame may create unseen key",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,1001U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,50U));

    CHECK("unseen key inserted",
          nrfclaw_ninalink_state_cache_get_node(A,&node) &&
          node.value_count==2U);

    nrfclaw_ninalink_state_cache_clear();
    set_s16(&e, 0x0100U, 0U, 3000);
    CHECK("wrap seed 65535",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,65535U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,60U));

    set_s16(&e, 0x0100U, 0U, 3001);
    CHECK("wrap seq0",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,0U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,70U));

    CHECK("wrap accepted",
          nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) &&
          value.value.v.s16==3001 &&
          value.last_sequence==0U &&
          value.update_count==2U);

    set_s16(&e, 0x0100U, 0U, 1234);
    CHECK("half-range frame accepted",
          nrfclaw_ninalink_state_cache_ingest_values(
              A,0x8000U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,80U));

    CHECK("half-range ignored",
          nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) &&
          value.value.v.s16==3001 &&
          value.last_sequence==0U &&
          value.update_count==2U);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
