#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ndp_key_store.h"
#include "nrfclaw_sha256.h"
#include "nrf_error.h"
#include "nrf_soc.h"
#include <string.h>

static nrfclaw_ndp_access_level_t m_level;
static uint8_t m_challenge[12];
static uint8_t m_pending_level;
static uint8_t m_session_id;
static bool m_pending;

static void hmac_sha256(const uint8_t *key, uint32_t key_len,
                        const uint8_t *msg, uint32_t msg_len,
                        uint8_t out[32])
{
    uint8_t k0[64]={0}, ipad[64], opad[64], inner[32], tmp[64+32];
    if (key_len > 64U) {
        nrfclaw_sha256(key,key_len,k0);
    } else memcpy(k0,key,key_len);
    for(uint32_t i=0;i<64U;i++){ ipad[i]=(uint8_t)(k0[i]^0x36U); opad[i]=(uint8_t)(k0[i]^0x5cU); }
    uint8_t ibuf[64+16];
    memcpy(ibuf,ipad,64); memcpy(&ibuf[64],msg,msg_len);
    nrfclaw_sha256(ibuf,64U+msg_len,inner);
    memcpy(tmp,opad,64); memcpy(&tmp[64],inner,32);
    nrfclaw_sha256(tmp,96U,out);
    memset(k0,0,sizeof(k0)); memset(ipad,0,sizeof(ipad)); memset(opad,0,sizeof(opad));
    memset(inner,0,sizeof(inner)); memset(tmp,0,sizeof(tmp)); memset(ibuf,0,sizeof(ibuf));
}

static bool random_bytes(uint8_t *out, uint8_t len)
{
    if(!out || len==0U) return false;

    while(len)
    {
        uint8_t available=0U;
        uint32_t err=sd_rand_application_bytes_available_get(&available);

        if(err!=NRF_SUCCESS)
            return false;

        if(available==0U)
            continue;

        uint8_t n=(available<len)?available:len;

        err=sd_rand_application_vector_get(out,n);
        if(err!=NRF_SUCCESS)
            return false;

        out+=n;
        len=(uint8_t)(len-n);
    }

    return true;
}

void nrfclaw_ndp_access_init(void){ m_level=NRFCLAW_NDP_PUBLIC; m_pending=false; m_session_id=0U; memset(m_challenge,0,sizeof(m_challenge)); }
void nrfclaw_ndp_access_on_disconnect(void){ nrfclaw_ndp_access_init(); }
nrfclaw_ndp_access_level_t nrfclaw_ndp_access_level(void){ return m_level; }
bool nrfclaw_ndp_access_allowed(nrfclaw_ndp_access_level_t required)
{
    /* Backward-compatible default: until the owner provisions an NDP key,
     * the Application/NDP plane remains open exactly as pre-R3.7 firmware.
     */
    if(!nrfclaw_ndp_key_configured()) return true;
    return m_level>=required;
}

bool nrfclaw_ndp_access_begin(uint8_t requested_level,uint8_t challenge[12],uint8_t *session_id)
{
    if(!nrfclaw_ndp_key_configured()) return false;
    if(!challenge||!session_id||requested_level<NRFCLAW_NDP_CONTROL||requested_level>NRFCLAW_NDP_PROVISION) return false;
    if(!random_bytes(m_challenge,sizeof(m_challenge)))
        return false;

    if(!random_bytes(&m_session_id,1U)) {
        memset(m_challenge,0,sizeof(m_challenge));
        return false;
    }

    if(m_session_id==0U) m_session_id=1U;
    m_pending_level=requested_level; m_pending=true;
    memcpy(challenge,m_challenge,sizeof(m_challenge)); *session_id=m_session_id;
    return true;
}

bool nrfclaw_ndp_access_finish(const uint8_t tag16[16])
{
    if(!m_pending||!tag16) return false;
    uint8_t msg[14], expected[32], diff=0U;
    uint8_t requested_level=m_pending_level;
    uint8_t session_id=m_session_id;
    memcpy(msg,m_challenge,12); msg[12]=requested_level; msg[13]=session_id;
    const uint8_t *key=nrfclaw_ndp_key();
    if(!key){
        memset(expected,0,sizeof(expected));
        memset(m_challenge,0,sizeof(m_challenge));
        m_pending=false;
        return false;
    }
    hmac_sha256(key,NRFCLAW_NDP_KEY_SIZE,msg,sizeof(msg),expected);
    for(uint8_t i=0;i<16U;i++) diff|=(uint8_t)(expected[i]^tag16[i]);
    memset(expected,0,sizeof(expected)); memset(m_challenge,0,sizeof(m_challenge)); m_pending=false;
    if(diff!=0U) return false;
    m_level=(nrfclaw_ndp_access_level_t)requested_level;
    return true;
}
