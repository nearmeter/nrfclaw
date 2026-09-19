#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "nrfclaw_ninalink_state_cache.h"

static unsigned g_tests;
static unsigned g_failures;
#define CHECK(label, cond) do { \
    g_tests++; \
    if (cond) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

static void set_u16(nrfclaw_ninalink_value_entry_t *e, uint16_t id, uint16_t v)
{ memset(e,0,sizeof(*e)); e->capability_id=id; e->value.type=NRFCLAW_CAP_VALUE_U16; e->value.v.u16=v; }
static void set_s16(nrfclaw_ninalink_value_entry_t *e, uint16_t id, int16_t v)
{ memset(e,0,sizeof(*e)); e->capability_id=id; e->value.type=NRFCLAW_CAP_VALUE_S16; e->value.v.s16=v; }
static void set_enum(nrfclaw_ninalink_value_entry_t *e, uint16_t id, uint8_t v)
{ memset(e,0,sizeof(*e)); e->capability_id=id; e->value.type=NRFCLAW_CAP_VALUE_ENUM8; e->value.v.u8=v; }

int main(void)
{
    const uint32_t A=0xAD64D423UL, B=0x11112222UL;
    nrfclaw_ninalink_value_entry_t e[2];
    nrfclaw_ninalink_state_cache_status_t st;
    nrfclaw_ninalink_state_cache_node_t node;
    nrfclaw_ninalink_cached_value_t value;
    nrfclaw_ninalink_cached_event_t event;
    nrfclaw_ninalink_event_history_status_t est;
    unsigned i;

    nrfclaw_ninalink_state_cache_clear();
    nrfclaw_ninalink_state_cache_get_status(&st);
    CHECK("clear", st.nodes==0U && st.values==0U);

    set_u16(&e[0],0x0001U,3310U); set_s16(&e[1],0x0100U,1975);
    CHECK("first report", nrfclaw_ninalink_state_cache_ingest_values(A,1U,NRFCLAW_NINALINK_MSG_CAP_REPORT,e,2U,10U));
    nrfclaw_ninalink_state_cache_get_status(&st);
    CHECK("first status", st.nodes==1U && st.values==2U && st.value_updates==2U);
    CHECK("node summary", nrfclaw_ninalink_state_cache_get_node(A,&node) && node.reports==1U && node.events==0U);
    CHECK("battery", nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) && value.capability_id==0x0001U && nrfclaw_ninalink_state_cache_value_raw(&value.value)==3310U);

    set_u16(&e[0],0x0001U,3290U);
    CHECK("update report", nrfclaw_ninalink_state_cache_ingest_values(A,2U,NRFCLAW_NINALINK_MSG_CAP_REPORT,e,1U,20U));
    CHECK("update count", nrfclaw_ninalink_state_cache_get_value_at(A,0U,&value) && value.update_count==2U && nrfclaw_ninalink_state_cache_value_raw(&value.value)==3290U);

    set_enum(&e[0],0x0201U,2U);
    CHECK("event", nrfclaw_ninalink_state_cache_ingest_values(A,3U,NRFCLAW_NINALINK_MSG_CAP_EVENT,e,1U,30U));
    CHECK("event summary", nrfclaw_ninalink_state_cache_get_node(A,&node) && node.reports==2U && node.events==1U && node.last_message_type==NRFCLAW_NINALINK_MSG_CAP_EVENT);
    CHECK("event not persistent state", nrfclaw_ninalink_state_cache_get_node(A,&node) && node.value_count==2U);
    nrfclaw_ninalink_event_history_get_status(&est);
    CHECK("event history status", est.entries==1U && est.ingested==1U && est.dropped==0U);
    CHECK("event node count", nrfclaw_ninalink_event_history_count(A)==1U);
    CHECK("event history value", nrfclaw_ninalink_event_history_get_at(A,0U,&event) && event.capability_id==0x0201U && event.value.type==NRFCLAW_CAP_VALUE_ENUM8 && event.value.v.u8==2U && event.sequence==3U);

    for(i=0U;i<NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE+1U;i++) {
        set_u16(&e[0],(uint16_t)(0x8000U+i),(uint16_t)i);
        CHECK("bounded ingest", nrfclaw_ninalink_state_cache_ingest_values(B,(uint16_t)(100U+i),NRFCLAW_NINALINK_MSG_CAP_REPORT,e,1U,(uint32_t)(100U+i)));
    }
    nrfclaw_ninalink_state_cache_get_status(&st);
    CHECK("value eviction", st.value_evictions==1U);
    CHECK("value cap", nrfclaw_ninalink_state_cache_get_node(B,&node) && node.value_count==NRFCLAW_NINALINK_STATE_CACHE_VALUES_PER_NODE);

    e[0].value.type=NRFCLAW_CAP_VALUE_S32; e[0].value.v.s32=-123456;
    CHECK("signed raw", nrfclaw_ninalink_state_cache_value_raw(&e[0].value)==(uint32_t)(int32_t)-123456);
    printf("\n%u test(s), %u failure(s)\n",g_tests,g_failures);
    return g_failures?1:0;
}
