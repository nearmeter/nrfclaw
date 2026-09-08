#include "nrfclaw_ndp_key_store.h"
#include "nrf.h"
#include "nrf_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"
#include <string.h>

#define KEY_MAGIC       0x314B444EUL /* "NDK1" */
#define KEY_FORMAT      0x00010000UL
#define KEY_COMMIT      0x544D4F43UL /* "COMT" */
#define KEY_ERASED      0xFFFFFFFFUL

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t generation;
    uint32_t key_len;
    uint32_t crc32;
    uint32_t commit;
    uint8_t  key[NRFCLAW_NDP_KEY_SIZE];
} key_image_t;

typedef enum {
    ST_IDLE=0,
    ST_ERASE_PENDING, ST_ERASE_WAIT,
    ST_BODY_PENDING, ST_BODY_WAIT,
    ST_COMMIT_PENDING, ST_COMMIT_WAIT,
    ST_VERIFY, ST_ERROR
} key_store_state_t;

#define KEY_IMAGE_WORDS ((uint32_t)(sizeof(key_image_t)/4U))
#define KEY_COMMIT_WORD_INDEX 5U

static key_store_state_t m_state;
static uint8_t m_active_slot;
static uint8_t m_target_slot;
static uint32_t m_generation;
static uint32_t m_target_addr;
static key_image_t m_stage;
static uint32_t m_commit_word=KEY_COMMIT;
static uint8_t m_active_key[NRFCLAW_NDP_KEY_SIZE];
static bool m_configured;
static volatile bool m_evt_ok,m_evt_err;

static uint32_t crc32_calc(uint8_t const *data,uint32_t len)
{
    uint32_t crc=0xFFFFFFFFUL;
    for(uint32_t i=0;i<len;i++){
        crc^=data[i];
        for(uint8_t b=0;b<8;b++)crc=(crc>>1)^((crc&1U)?0xEDB88320UL:0U);
    }
    return crc^0xFFFFFFFFUL;
}

static uint32_t slot_addr(uint8_t slot)
{
    return slot ? NRFCLAW_NDP_KEY_SLOT_B_ADDR : NRFCLAW_NDP_KEY_SLOT_A_ADDR;
}

static bool gen_newer(uint32_t a,uint32_t b){return (int32_t)(a-b)>0;}

static bool geometry_ok(void)
{
    uint32_t total=NRF_FICR->CODESIZE*NRF_FICR->CODEPAGESIZE;
    return NRF_FICR->CODEPAGESIZE==4096U &&
           total>NRFCLAW_NDP_KEY_SLOT_B_ADDR &&
           (NRFCLAW_NDP_KEY_SLOT_A_ADDR%4096U)==0U &&
           (NRFCLAW_NDP_KEY_SLOT_B_ADDR%4096U)==0U;
}

static bool slot_valid(uint8_t slot,key_image_t *out,uint32_t *gen)
{
    key_image_t const *h=(key_image_t const *)(uintptr_t)slot_addr(slot);
    if(h->magic!=KEY_MAGIC||h->format!=KEY_FORMAT||h->commit!=KEY_COMMIT)
        return false;
    if(h->key_len!=NRFCLAW_NDP_KEY_SIZE)
        return false;
    if(crc32_calc(h->key,NRFCLAW_NDP_KEY_SIZE)!=h->crc32)
        return false;
    if(out) memcpy(out,h,sizeof(*out));
    if(gen) *gen=h->generation;
    return true;
}

static bool random_bytes(uint8_t *out,uint8_t len)
{
    if(!out||len==0U) return false;
    while(len){
        uint8_t available=0U;
        uint32_t err=sd_rand_application_bytes_available_get(&available);
        if(err!=NRF_SUCCESS) return false;
        if(available==0U) continue;
        uint8_t n=(available<len)?available:len;
        err=sd_rand_application_vector_get(out,n);
        if(err!=NRF_SUCCESS) return false;
        out+=n; len=(uint8_t)(len-n);
    }
    return true;
}

static void soc_evt(uint32_t e,void *ctx)
{
    (void)ctx;
    if(m_state!=ST_ERASE_WAIT&&m_state!=ST_BODY_WAIT&&m_state!=ST_COMMIT_WAIT)
        return;
    if(e==NRF_EVT_FLASH_OPERATION_SUCCESS)m_evt_ok=true;
    else if(e==NRF_EVT_FLASH_OPERATION_ERROR)m_evt_err=true;
}

#ifndef NRFCLAW_NDP_KEY_STORE_SOC_OBSERVER_PRIO
#define NRFCLAW_NDP_KEY_STORE_SOC_OBSERVER_PRIO 1
#endif

STATIC_ASSERT(NRFCLAW_NDP_KEY_STORE_SOC_OBSERVER_PRIO <
              NRF_SDH_SOC_OBSERVER_PRIO_LEVELS,
              "NDP key store SoC observer priority unavailable");

NRF_SDH_SOC_OBSERVER(m_ndp_key_store_observer,
                     NRFCLAW_NDP_KEY_STORE_SOC_OBSERVER_PRIO,
                     soc_evt,
                     NULL);

void nrfclaw_ndp_key_store_init(void)
{
    m_state=ST_IDLE;
    m_active_slot=0xFFU;
    m_target_slot=0U;
    m_generation=0U;
    m_target_addr=0U;
    m_configured=false;
    m_evt_ok=m_evt_err=false;
    memset(m_active_key,0,sizeof(m_active_key));
    memset(&m_stage,0,sizeof(m_stage));

    if(!geometry_ok()){m_state=ST_ERROR;return;}

    key_image_t a,b;
    uint32_t ga=0,gb=0;
    bool va=slot_valid(0,&a,&ga),vb=slot_valid(1,&b,&gb);
    if(va&&(!vb||gen_newer(ga,gb))){
        m_active_slot=0U;m_generation=ga;memcpy(m_active_key,a.key,sizeof(m_active_key));m_configured=true;
    } else if(vb){
        m_active_slot=1U;m_generation=gb;memcpy(m_active_key,b.key,sizeof(m_active_key));m_configured=true;
    }
    memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));
}

bool nrfclaw_ndp_key_configured(void){return m_configured;}

nrfclaw_ndp_key_status_t nrfclaw_ndp_key_status(void)
{
    if(m_state==ST_ERROR) return NRFCLAW_NDP_KEY_ERROR;
    if(m_state!=ST_IDLE) return NRFCLAW_NDP_KEY_SAVING;
    return m_configured ? NRFCLAW_NDP_KEY_READY : NRFCLAW_NDP_KEY_UNCONFIGURED;
}

const uint8_t *nrfclaw_ndp_key(void)
{
    return m_configured ? m_active_key : NULL;
}

bool nrfclaw_ndp_key_generate(void)
{
    if(m_state!=ST_IDLE || !geometry_ok()) return false;

    memset(&m_stage,0xFF,sizeof(m_stage));
    m_stage.magic=KEY_MAGIC;
    m_stage.format=KEY_FORMAT;
    m_stage.generation=m_generation+1U;
    m_stage.key_len=NRFCLAW_NDP_KEY_SIZE;
    m_stage.commit=KEY_ERASED;
    if(!random_bytes(m_stage.key,NRFCLAW_NDP_KEY_SIZE)){
        memset(&m_stage,0,sizeof(m_stage));
        return false;
    }
    m_stage.crc32=crc32_calc(m_stage.key,NRFCLAW_NDP_KEY_SIZE);
    m_target_slot=(m_active_slot==0U)?1U:0U;
    if(m_active_slot>1U)m_target_slot=0U;
    m_target_addr=slot_addr(m_target_slot);
    m_evt_ok=m_evt_err=false;
    m_state=ST_ERASE_PENDING;
    return true;
}

void nrfclaw_ndp_key_store_process(void)
{
    if(m_state==ST_ERROR||m_state==ST_IDLE)return;
    if(m_evt_err){m_evt_err=false;m_state=ST_ERROR;return;}
    if(m_evt_ok){
        m_evt_ok=false;
        if(m_state==ST_ERASE_WAIT)m_state=ST_BODY_PENDING;
        else if(m_state==ST_BODY_WAIT)m_state=ST_COMMIT_PENDING;
        else if(m_state==ST_COMMIT_WAIT)m_state=ST_VERIFY;
    }

    if(m_state==ST_ERASE_PENDING){
        uint32_t e=sd_flash_page_erase(m_target_addr/NRF_FICR->CODEPAGESIZE);
        if(e==NRF_SUCCESS)m_state=ST_ERASE_WAIT;
        else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_BODY_PENDING){
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)m_target_addr,
                                  (uint32_t const *)&m_stage,
                                  KEY_IMAGE_WORDS);
        if(e==NRF_SUCCESS)m_state=ST_BODY_WAIT;
        else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_COMMIT_PENDING){
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)(m_target_addr+KEY_COMMIT_WORD_INDEX*4U),
                                  &m_commit_word,1U);
        if(e==NRF_SUCCESS)m_state=ST_COMMIT_WAIT;
        else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_VERIFY){
        key_image_t verified;
        uint32_t g=0;
        if(!slot_valid(m_target_slot,&verified,&g)){m_state=ST_ERROR;return;}
        memcpy(m_active_key,verified.key,sizeof(m_active_key));
        memset(&verified,0,sizeof(verified));
        m_active_slot=m_target_slot;
        m_generation=g;
        m_configured=true;
        memset(&m_stage,0,sizeof(m_stage));
        m_state=ST_IDLE;
    }
}
