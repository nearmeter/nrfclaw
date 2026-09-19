#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nrfclaw_ninalink_state_cache.h"
static unsigned t,f;
#define C(n,x) do{t++;if(x)printf("PASS %s\n",n);else{printf("FAIL %s\n",n);f++;}}while(0)
static void ev(nrfclaw_ninalink_value_entry_t *e,uint8_t v){memset(e,0,sizeof(*e));e->capability_id=0x0201U;e->value.type=NRFCLAW_CAP_VALUE_ENUM8;e->value.v.u8=v;}
int main(void){const uint32_t A=0xAD64D423UL;nrfclaw_ninalink_value_entry_t e;nrfclaw_ninalink_event_history_status_t s;nrfclaw_ninalink_cached_event_t x;unsigned i;
nrfclaw_ninalink_state_cache_clear();
for(i=0;i<10;i++){ev(&e,(uint8_t)i);C("ingest",nrfclaw_ninalink_state_cache_ingest_values(A,(uint16_t)(100+i),NRFCLAW_NINALINK_MSG_CAP_EVENT,&e,1U,1000+i));}
nrfclaw_ninalink_event_history_get_status(&s);C("depth8",s.entries==8U&&s.dropped==2U);C("window3to10",s.oldest_id==3U&&s.newest_id==10U);C("after0is3",nrfclaw_ninalink_event_history_get_after(0U,&x)&&x.event_id==3U);C("after3is4",nrfclaw_ninalink_event_history_get_after(3U,&x)&&x.event_id==4U);C("id1evicted",!nrfclaw_ninalink_event_history_get_by_id(1U,&x));C("id10present",nrfclaw_ninalink_event_history_get_by_id(10U,&x)&&x.event_id==10U);
nrfclaw_ninalink_state_cache_clear();nrfclaw_ninalink_event_history_get_status(&s);C("clear empty",s.entries==0U&&s.oldest_id==0U&&s.newest_id==0U);ev(&e,42U);C("postclear ingest",nrfclaw_ninalink_state_cache_ingest_values(A,200U,NRFCLAW_NINALINK_MSG_CAP_EVENT,&e,1U,2000U));C("id continues",nrfclaw_ninalink_event_history_get_after(10U,&x)&&x.event_id==11U);
printf("\n%u test(s), %u failure(s)\n",t,f);return f?1:0;}
