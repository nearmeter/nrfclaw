#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nrfclaw_ninalink_state_cache.h"

static unsigned tests,failures;
#define CHECK(n,e) do{tests++;if(e)printf("PASS %s\n",n);else{printf("FAIL %s\n",n);failures++;}}while(0)

static void temp(nrfclaw_ninalink_value_entry_t *e,int16_t v){
 memset(e,0,sizeof(*e)); e->capability_id=0x0100U;e->value.type=NRFCLAW_CAP_VALUE_S16;e->value.v.s16=v;
}
static void tap(nrfclaw_ninalink_value_entry_t *e){
 memset(e,0,sizeof(*e)); e->capability_id=0x0201U;e->value.type=NRFCLAW_CAP_VALUE_ENUM8;e->value.v.u8=2U;
}
int main(void){
 const uint32_t N=0xAD64D423UL,S=0x11111111UL;
 nrfclaw_ninalink_value_entry_t e; uint32_t sr,er,cr,s0,e0,c0;
 nrfclaw_ninalink_state_cache_clear();
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr);
 CHECK("initial revisions zero",sr==0U&&er==0U&&cr==0U);
 temp(&e,2000); CHECK("report seq1",nrfclaw_ninalink_state_cache_ingest_values_session(N,S,1U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,1U));
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr); s0=sr;e0=er;c0=cr;
 CHECK("state revision increments",sr==1U&&er==0U&&cr==1U);
 temp(&e,1500); CHECK("equal seq consumed",nrfclaw_ninalink_state_cache_ingest_values_session(N,S,1U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,2U));
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr);
 CHECK("stale/equal report no external state revision",sr==s0&&er==e0&&cr==c0);
 temp(&e,2100); CHECK("new report seq2",nrfclaw_ninalink_state_cache_ingest_values_session(N,S,2U,NRFCLAW_NINALINK_MSG_CAP_REPORT,&e,1U,3U));
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr);
 CHECK("newer report state revision once",sr==s0+1U&&er==e0&&cr==c0+1U);
 tap(&e); CHECK("event seq3",nrfclaw_ninalink_state_cache_ingest_values_session(N,S,3U,NRFCLAW_NINALINK_MSG_CAP_EVENT,&e,1U,4U));
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr); s0=sr;e0=er;c0=cr;
 CHECK("event revision increments",er==1U&&cr==3U);
 nrfclaw_ninalink_state_cache_clear();
 nrfclaw_ninalink_state_cache_get_revisions(&sr,&er,&cr);
 CHECK("clear bumps nonempty state",sr==s0+1U);
 CHECK("clear bumps nonempty event",er==e0+1U);
 CHECK("revisions never reset on cache clear",cr>c0);
 printf("\n%u test(s), %u failure(s)\n",tests,failures);return failures?1:0;
}
