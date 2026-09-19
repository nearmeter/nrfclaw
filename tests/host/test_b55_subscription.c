#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nrfclaw_ble_app.h"
#include "nrfclaw_ninalink_external.h"
#include "nrfclaw_ninalink_external_subscription.h"

static unsigned tests,failures; static uint32_t sr,er,cr,newest,txgen; static bool connected=true;
static nrfclaw_ble_app_status_t send_status=NRFCLAW_BLE_APP_OK; static uint8_t last[20]; static unsigned sends;
#define CHECK(n,e) do{tests++;if(e)printf("PASS %s\n",n);else{printf("FAIL %s\n",n);failures++;}}while(0)
void nrfclaw_ninalink_state_cache_get_revisions(uint32_t *s,uint32_t *e,uint32_t *c){if(s)*s=sr;if(e)*e=er;if(c)*c=cr;}
void nrfclaw_ninalink_external_get_status(nrfclaw_ninalink_external_status_t *o){memset(o,0,sizeof(*o));o->schema_version=1;o->newest_event_id=newest;}
bool nrfclaw_ble_app_connected(void){return connected;}
uint32_t nrfclaw_ble_app_tx_generation(void){return txgen;}
nrfclaw_ble_app_status_t nrfclaw_ble_app_send_raw(const uint8_t *d,uint16_t n){sends++;if(n==20)memcpy(last,d,20);return send_status;}
int main(void){nrfclaw_ninalink_change_status_t st;nrfclaw_ninalink_change_stats_t stats;
 nrfclaw_ninalink_external_subscription_set(3U); nrfclaw_ninalink_external_subscription_process(); CHECK("subscribe has no synthetic change",sends==0U);
 sr=1;cr=1;nrfclaw_ninalink_external_subscription_process();CHECK("state notify sent",sends==1U&&last[0]==0xE5&&last[2]==1U);
 send_status=NRFCLAW_BLE_APP_RESOURCES;er=1;cr=2;newest=7;nrfclaw_ninalink_external_subscription_process();CHECK("busy retains event",sends==2U);
 nrfclaw_ninalink_external_subscription_process();CHECK("no busy spin without TX progress",sends==2U);
 txgen++;send_status=NRFCLAW_BLE_APP_OK;nrfclaw_ninalink_external_subscription_process();CHECK("event retry after TX progress",sends==3U&&last[2]==2U&&last[12]==7U);
 send_status=NRFCLAW_BLE_APP_RESOURCES;sr=2;er=2;cr=4;newest=8;nrfclaw_ninalink_external_subscription_process();unsigned before=sends;sr=3;cr=5;nrfclaw_ninalink_external_subscription_process();CHECK("coalescing does not spin",sends==before);
 txgen++;send_status=NRFCLAW_BLE_APP_OK;nrfclaw_ninalink_external_subscription_process();CHECK("coalesced state+event latest revisions",last[2]==3U&&last[4]==3U&&last[8]==2U&&last[12]==8U);
 nrfclaw_ninalink_external_subscription_get_stats(&stats);CHECK("coalesced diagnostic increments",stats.coalesced>=1U&&stats.busy_retries>=2U);
 connected=false;nrfclaw_ninalink_external_subscription_process();nrfclaw_ninalink_external_subscription_get_status(&st);CHECK("disconnect clears subscription",st.mask==0U);
 printf("\n%u test(s), %u failure(s)\n",tests,failures);return failures?1:0;}
