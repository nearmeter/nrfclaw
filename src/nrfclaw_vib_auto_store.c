#include "nrfclaw_vib_auto_store.h"
#include "nrfclaw_board.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"
#include <string.h>

#ifndef NRFCLAW_EXPERIMENTAL_VIB_AUTO
#define NRFCLAW_EXPERIMENTAL_VIB_AUTO 0
#endif

#define VA_STORE_MAGIC   0x3256414EUL /* "NAV2" little-endian-ish marker */
#define VA_STORE_FORMAT  0x00020003UL
#define VA_STORE_FORMAT_LEGACY 0x00020002UL
#define VA_STORE_COMMIT  0x544D4F43UL /* COMT */
#define VA_STORE_ERASED  0xFFFFFFFFUL


/* r3.8.8 migration reader for the r3.8.5-r3.8.7 persisted payload.
 * Keep this layout frozen: arming lived in confirm_delay_s (uint8_t). */
typedef struct {
    uint32_t learning_time_s;
    uint16_t discovery_interval_s;
    uint16_t normal_interval_s;
    uint16_t suspicious_interval_s;
    uint16_t wake_threshold_mg;
    uint16_t wake_duration_ms;
    uint16_t off_rms_mg;
    uint16_t profile_match_q8_8;
    uint16_t adapt_limit_q8_8;
    uint8_t  confirm_delay_s;
    uint8_t  candidate_confirmations;
    uint8_t  alarm_consecutive;
    uint8_t  max_profiles;
    uint8_t  sensitivity;
    uint8_t  adaptation_shift;
    uint8_t  stable_expand_after;
    uint8_t  max_interval_multiplier;
} va_legacy_config_t;

typedef struct {
    uint8_t enabled;
    uint8_t discovery_complete;
    uint8_t profile_count;
    uint8_t reserved0;
    uint16_t quiet_rms_mg;
    uint16_t quiet_peak_mg;
    va_legacy_config_t config;
    nrfclaw_vib_auto_profile_t profiles[NRFCLAW_VIB_AUTO_MAX_PROFILES];
} va_legacy_persist_t;

static void migrate_legacy(va_legacy_persist_t const *old, nrfclaw_vib_auto_persist_t *out){
    memset(out,0,sizeof(*out));
    out->enabled=old->enabled;out->discovery_complete=old->discovery_complete;out->profile_count=old->profile_count;
    out->quiet_rms_mg=old->quiet_rms_mg;out->quiet_peak_mg=old->quiet_peak_mg;
    out->config.learning_time_s=old->config.learning_time_s;
    out->config.discovery_interval_s=old->config.discovery_interval_s;
    out->config.normal_interval_s=old->config.normal_interval_s;
    out->config.suspicious_interval_s=old->config.suspicious_interval_s;
    out->config.wake_threshold_mg=old->config.wake_threshold_mg;
    out->config.wake_duration_ms=old->config.wake_duration_ms;
    out->config.off_rms_mg=old->config.off_rms_mg;
    out->config.profile_match_q8_8=old->config.profile_match_q8_8;
    out->config.adapt_limit_q8_8=old->config.adapt_limit_q8_8;
    out->config.confirm_delay_s=old->config.confirm_delay_s;
    out->config.candidate_confirmations=old->config.candidate_confirmations;
    out->config.alarm_consecutive=old->config.alarm_consecutive;
    out->config.max_profiles=old->config.max_profiles;
    out->config.sensitivity=old->config.sensitivity;
    out->config.adaptation_shift=old->config.adaptation_shift;
    out->config.stable_expand_after=old->config.stable_expand_after;
    out->config.max_interval_multiplier=old->config.max_interval_multiplier;
    out->config.arming_delay_s=old->config.confirm_delay_s;
    for(uint8_t i=0;i<NRFCLAW_VIB_AUTO_MAX_PROFILES;i++)out->profiles[i]=old->profiles[i];
}

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t crc32;
    uint32_t commit;
} va_store_header_t;

#define VA_STORE_HEADER_WORDS ((uint32_t)(sizeof(va_store_header_t)/4U))
#define VA_STORE_PAYLOAD_WORDS ((uint32_t)((sizeof(nrfclaw_vib_auto_persist_t)+3U)/4U))
#define VA_STORE_IMAGE_WORDS (VA_STORE_HEADER_WORDS + VA_STORE_PAYLOAD_WORDS)
#define VA_STORE_COMMIT_WORD_INDEX 5U

typedef enum {
    ST_IDLE=0,
    ST_ERASE_PENDING, ST_ERASE_WAIT,
    ST_BODY_PENDING, ST_BODY_WAIT,
    ST_COMMIT_PENDING, ST_COMMIT_WAIT,
    ST_VERIFY, ST_ERROR
} va_store_state_t;

static va_store_state_t m_state;
static uint8_t m_active_slot;
static uint8_t m_target_slot;
static uint32_t m_generation;
static uint32_t m_target_addr;
static uint32_t m_stage[VA_STORE_IMAGE_WORDS];
static uint32_t m_commit_word=VA_STORE_COMMIT;
static volatile bool m_evt_ok,m_evt_err;
static bool m_have_pending;
static nrfclaw_vib_auto_persist_t m_pending;

static uint32_t crc32_calc(uint8_t const *data,uint32_t len){
    uint32_t crc=0xFFFFFFFFUL;
    for(uint32_t i=0;i<len;i++){
        crc^=data[i];
        for(uint8_t b=0;b<8;b++)crc=(crc>>1)^((crc&1U)?0xEDB88320UL:0U);
    }
    return crc^0xFFFFFFFFUL;
}

static uint32_t slot_addr(uint8_t slot){return slot?NRFCLAW_VIB_AUTO_STORE_SLOT_B_ADDR:NRFCLAW_VIB_AUTO_STORE_SLOT_A_ADDR;}
static bool gen_newer(uint32_t a,uint32_t b){return (int32_t)(a-b)>0;}

static bool geometry_ok(void){
    uint32_t total=NRF_FICR->CODESIZE*NRF_FICR->CODEPAGESIZE;
    return NRF_FICR->CODEPAGESIZE==4096U && total>NRFCLAW_VIB_AUTO_STORE_SLOT_B_ADDR &&
           (NRFCLAW_VIB_AUTO_STORE_SLOT_A_ADDR%4096U)==0U && (NRFCLAW_VIB_AUTO_STORE_SLOT_B_ADDR%4096U)==0U;
}

static bool slot_valid(uint8_t slot,nrfclaw_vib_auto_persist_t *out,uint32_t *gen){
    uint32_t addr=slot_addr(slot);
    va_store_header_t const *h=(va_store_header_t const *)(uintptr_t)addr;
    if(h->magic!=VA_STORE_MAGIC||h->commit!=VA_STORE_COMMIT)return false;
    uint8_t const *p=(uint8_t const *)(uintptr_t)(addr+sizeof(va_store_header_t));
    if(h->format==VA_STORE_FORMAT){
        if(h->payload_len!=sizeof(nrfclaw_vib_auto_persist_t))return false;
        if(crc32_calc(p,h->payload_len)!=h->crc32)return false;
        if(out)memcpy(out,p,sizeof(*out));
    }else if(h->format==VA_STORE_FORMAT_LEGACY){
        if(h->payload_len!=sizeof(va_legacy_persist_t))return false;
        if(crc32_calc(p,h->payload_len)!=h->crc32)return false;
        if(out){va_legacy_persist_t legacy;memcpy(&legacy,p,sizeof(legacy));migrate_legacy(&legacy,out);}
    }else return false;
    if(gen)*gen=h->generation;
    return true;
}

static void soc_evt(uint32_t e,void *ctx){
    (void)ctx;
    if(m_state!=ST_ERASE_WAIT&&m_state!=ST_BODY_WAIT&&m_state!=ST_COMMIT_WAIT)return;
    if(e==NRF_EVT_FLASH_OPERATION_SUCCESS)m_evt_ok=true;
    else if(e==NRF_EVT_FLASH_OPERATION_ERROR)m_evt_err=true;
}
/*
 * The standalone nRFClaw sdk_config currently exposes only two SoC observer
 * priority levels (0 and 1). Sharing a priority with another observer is
 * supported; the priority controls ordering, not exclusivity.
 */
#ifndef NRFCLAW_VIB_AUTO_STORE_SOC_OBSERVER_PRIO
#define NRFCLAW_VIB_AUTO_STORE_SOC_OBSERVER_PRIO 1
#endif

STATIC_ASSERT(NRFCLAW_VIB_AUTO_STORE_SOC_OBSERVER_PRIO <
              NRF_SDH_SOC_OBSERVER_PRIO_LEVELS,
              "VIB_AUTO store SoC observer priority unavailable");

NRF_SDH_SOC_OBSERVER(m_vib_auto_store_observer,
                     NRFCLAW_VIB_AUTO_STORE_SOC_OBSERVER_PRIO,
                     soc_evt,
                     NULL);

void nrfclaw_vib_auto_store_init(void){
    m_state=ST_IDLE;m_active_slot=0xFFU;m_target_slot=0U;m_generation=0U;m_evt_ok=m_evt_err=false;m_have_pending=false;
#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
    if(!geometry_ok()){m_state=ST_ERROR;return;}
    uint32_t ga=0,gb=0;bool va=slot_valid(0,NULL,&ga),vb=slot_valid(1,NULL,&gb);
    if(va&&(!vb||gen_newer(ga,gb))){m_active_slot=0;m_generation=ga;}
    else if(vb){m_active_slot=1;m_generation=gb;}
#endif
}

bool nrfclaw_vib_auto_store_load(nrfclaw_vib_auto_persist_t *out){
#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
    if(!out||m_active_slot>1U)return false;
    return slot_valid(m_active_slot,out,NULL);
#else
    (void)out;return false;
#endif
}

bool nrfclaw_vib_auto_store_request_save(nrfclaw_vib_auto_persist_t const *in){
#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
    if(!in||m_state==ST_ERROR)return false;
    /* Coalesce updates while a previous save is active. */
    m_pending=*in;m_have_pending=true;return true;
#else
    (void)in;return false;
#endif
}

static void start_pending(void){
    if(!m_have_pending||m_state!=ST_IDLE)return;
    memset(m_stage,0xFF,sizeof(m_stage));
    va_store_header_t *h=(va_store_header_t *)m_stage;
    h->magic=VA_STORE_MAGIC;h->format=VA_STORE_FORMAT;h->generation=m_generation+1U;
    h->payload_len=sizeof(nrfclaw_vib_auto_persist_t);
    h->crc32=crc32_calc((uint8_t const *)&m_pending,sizeof(m_pending));
    h->commit=VA_STORE_ERASED;
    memcpy((uint8_t *)m_stage+sizeof(*h),&m_pending,sizeof(m_pending));
    m_target_slot=(m_active_slot==0U)?1U:0U;if(m_active_slot>1U)m_target_slot=0U;
    m_target_addr=slot_addr(m_target_slot);m_have_pending=false;m_evt_ok=m_evt_err=false;m_state=ST_ERASE_PENDING;
}

void nrfclaw_vib_auto_store_process(void){
#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
    if(m_state==ST_ERROR)return;
    if(m_evt_err){m_evt_err=false;m_state=ST_ERROR;return;}
    if(m_evt_ok){
        m_evt_ok=false;
        if(m_state==ST_ERASE_WAIT)m_state=ST_BODY_PENDING;
        else if(m_state==ST_BODY_WAIT)m_state=ST_COMMIT_PENDING;
        else if(m_state==ST_COMMIT_WAIT)m_state=ST_VERIFY;
    }
    if(m_state==ST_IDLE){start_pending();return;}
    if(m_state==ST_ERASE_PENDING){
        uint32_t e=sd_flash_page_erase(m_target_addr/NRF_FICR->CODEPAGESIZE);
        if(e==NRF_SUCCESS)m_state=ST_ERASE_WAIT;else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_BODY_PENDING){
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)m_target_addr,m_stage,VA_STORE_IMAGE_WORDS);
        if(e==NRF_SUCCESS)m_state=ST_BODY_WAIT;else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_COMMIT_PENDING){
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)(m_target_addr+VA_STORE_COMMIT_WORD_INDEX*4U),&m_commit_word,1U);
        if(e==NRF_SUCCESS)m_state=ST_COMMIT_WAIT;else if(e!=NRF_ERROR_BUSY)m_state=ST_ERROR;
    } else if(m_state==ST_VERIFY){
        uint32_t g=0;
        if(!slot_valid(m_target_slot,NULL,&g)){m_state=ST_ERROR;return;}
        m_active_slot=m_target_slot;m_generation=g;m_state=ST_IDLE;start_pending();
    }
#endif
}

bool nrfclaw_vib_auto_store_busy(void){return m_state!=ST_IDLE||m_have_pending;}
uint32_t nrfclaw_vib_auto_store_generation(void){return m_generation;}
