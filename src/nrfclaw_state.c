#include "nrfclaw_state.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include <string.h>

#define REC_MAGIC 0x3854534EUL /* "NST8" */
#define REC_TAG   0xA55AU
#define REC_WORDS 5U
#define REC_SIZE  (REC_WORDS * 4U)
#define PAGE_SIZE 4096U
#define REC_PER_PAGE (PAGE_SIZE / REC_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t key;
    uint32_t value;
    uint32_t footer; /* high16 tag, low16 crc */
} state_rec_t;

typedef enum {
    WR_IDLE=0, WR_WRITE_PENDING, WR_WRITE_WAIT,
    WR_COMPACT_ERASE_PENDING, WR_COMPACT_ERASE_WAIT,
    WR_COMPACT_WRITE_PENDING, WR_COMPACT_WRITE_WAIT,
    WR_ERROR
} wr_state_t;

static uint32_t m_value[NRFCLAW_STATE_COUNT];
static bool m_valid[NRFCLAW_STATE_COUNT];
static uint32_t m_seq;
static uint8_t m_active_page;
static uint16_t m_next_index;
static wr_state_t m_wr;
static nrfclaw_state_status_t m_status;
static state_rec_t m_pending;
static volatile bool m_evt_ok, m_evt_err;
static uint8_t m_compact_key;

static uint16_t crc16(uint8_t const *p, uint16_t n)
{
    uint16_t c=0xFFFFU;
    while(n--) { c ^= (uint16_t)(*p++)<<8; for(uint8_t b=0;b<8;b++) c=(c&0x8000U)?(uint16_t)((c<<1)^0x1021U):(uint16_t)(c<<1); }
    return c;
}
static uint32_t page_addr(uint8_t p){ return p?NRFCLAW_STATE_PAGE1_ADDR:NRFCLAW_STATE_PAGE0_ADDR; }
static bool rec_valid(state_rec_t const *r)
{
    if(r->magic!=REC_MAGIC || (r->footer>>16)!=REC_TAG || r->key>=NRFCLAW_STATE_COUNT) return false;
    return (uint16_t)r->footer == crc16((uint8_t const*)r,16U);
}
static void build_rec(state_rec_t *r,uint8_t key,uint32_t value,uint32_t seq)
{
    r->magic=REC_MAGIC; r->seq=seq; r->key=key; r->value=value;
    r->footer=((uint32_t)REC_TAG<<16)|crc16((uint8_t const*)r,16U);
}
static void soc_evt(uint32_t e,void *ctx){(void)ctx; if(e==NRF_EVT_FLASH_OPERATION_SUCCESS)m_evt_ok=true; else if(e==NRF_EVT_FLASH_OPERATION_ERROR)m_evt_err=true;}
/*
 * The project exposes SOC observer priorities 0..1.
 * Program dual-slot Flash already uses priority 1, so the state journal
 * uses priority 0. Both still receive the global SoftDevice Flash events;
 * each state machine only consumes SUCCESS/ERROR while its own accepted
 * flash operation is in a WAIT state.
 */
NRF_SDH_SOC_OBSERVER(m_state_soc_observer,0,soc_evt,NULL);

void nrfclaw_state_init(void)
{
    memset(m_value,0,sizeof(m_value)); memset(m_valid,0,sizeof(m_valid));
    m_seq=0; m_active_page=0; m_next_index=0; m_wr=WR_IDLE; m_status=NRFCLAW_STATE_IDLE; m_evt_ok=m_evt_err=false;
    uint32_t best_page_seq[2]={0,0}; uint16_t free_index[2]={REC_PER_PAGE,REC_PER_PAGE};
    uint32_t key_seq[NRFCLAW_STATE_COUNT]={0};
    for(uint8_t p=0;p<2;p++){
        state_rec_t const *base=(state_rec_t const*)(uintptr_t)page_addr(p);
        for(uint16_t i=0;i<REC_PER_PAGE;i++){
            state_rec_t const *r=&base[i];
            if(r->magic==0xFFFFFFFFUL){ if(free_index[p]==REC_PER_PAGE) free_index[p]=i; continue; }
            if(!rec_valid(r)) continue;
            if(r->seq>best_page_seq[p]) best_page_seq[p]=r->seq;
            uint8_t k=(uint8_t)r->key;
            /* Full scan in increasing page/index; sequence decides newest. */
            if(!m_valid[k] || r->seq>key_seq[k]){ key_seq[k]=r->seq; m_value[k]=r->value; m_valid[k]=true; }
            if(r->seq>m_seq)m_seq=r->seq;
        }
    }
    m_active_page=(best_page_seq[1]>best_page_seq[0])?1U:0U;
    m_next_index=free_index[m_active_page];
    if(m_next_index>=REC_PER_PAGE){ m_wr=WR_COMPACT_ERASE_PENDING; m_status=NRFCLAW_STATE_SAVING; m_compact_key=0; }
}

bool nrfclaw_state_set(uint8_t key,uint32_t value){ if(key>=NRFCLAW_STATE_COUNT)return false; m_value[key]=value; m_valid[key]=true; return true; }
bool nrfclaw_state_get(uint8_t key,uint32_t *value){ if(key>=NRFCLAW_STATE_COUNT||!value||!m_valid[key])return false; *value=m_value[key]; return true; }
bool nrfclaw_state_has(uint8_t key){ return key<NRFCLAW_STATE_COUNT && m_valid[key]; }

bool nrfclaw_state_persist(uint8_t key,uint32_t value)
{
    if(key>=NRFCLAW_STATE_COUNT || m_wr!=WR_IDLE) return false;
    nrfclaw_state_set(key,value);
    if(m_next_index>=REC_PER_PAGE){ m_wr=WR_COMPACT_ERASE_PENDING; m_status=NRFCLAW_STATE_SAVING; m_compact_key=0; return false; }
    build_rec(&m_pending,key,value,++m_seq); m_wr=WR_WRITE_PENDING; m_status=NRFCLAW_STATE_SAVING; return true;
}

void nrfclaw_state_process(void)
{
    if(m_evt_err){m_evt_err=false;m_wr=WR_ERROR;m_status=NRFCLAW_STATE_ERROR;return;}
    if(m_evt_ok){
        m_evt_ok=false;
        if(m_wr==WR_WRITE_WAIT){m_next_index++;m_wr=WR_IDLE;m_status=NRFCLAW_STATE_IDLE;}
        else if(m_wr==WR_COMPACT_ERASE_WAIT){m_next_index=0;m_compact_key=0;m_wr=WR_COMPACT_WRITE_PENDING;}
        else if(m_wr==WR_COMPACT_WRITE_WAIT){m_next_index++;m_compact_key++;m_wr=WR_COMPACT_WRITE_PENDING;}
    }
    if(m_wr==WR_WRITE_PENDING){
        uint32_t *dst=(uint32_t*)(uintptr_t)(page_addr(m_active_page)+(uint32_t)m_next_index*REC_SIZE);
        uint32_t e=sd_flash_write(dst,(uint32_t const*)&m_pending,REC_WORDS);
        if(e==NRF_SUCCESS)m_wr=WR_WRITE_WAIT; else if(e!=NRF_ERROR_BUSY){m_wr=WR_ERROR;m_status=NRFCLAW_STATE_ERROR;}
    } else if(m_wr==WR_COMPACT_ERASE_PENDING){
        uint8_t target=(uint8_t)(1U-m_active_page); uint32_t page=page_addr(target)/NRF_FICR->CODEPAGESIZE;
        uint32_t e=sd_flash_page_erase(page); if(e==NRF_SUCCESS){m_active_page=target;m_wr=WR_COMPACT_ERASE_WAIT;} else if(e!=NRF_ERROR_BUSY){m_wr=WR_ERROR;m_status=NRFCLAW_STATE_ERROR;}
    } else if(m_wr==WR_COMPACT_WRITE_PENDING){
        while(m_compact_key<NRFCLAW_STATE_COUNT && !m_valid[m_compact_key])m_compact_key++;
        if(m_compact_key>=NRFCLAW_STATE_COUNT){m_wr=WR_IDLE;m_status=NRFCLAW_STATE_IDLE;return;}
        build_rec(&m_pending,m_compact_key,m_value[m_compact_key],++m_seq);
        uint32_t *dst=(uint32_t*)(uintptr_t)(page_addr(m_active_page)+(uint32_t)m_next_index*REC_SIZE);
        uint32_t e=sd_flash_write(dst,(uint32_t const*)&m_pending,REC_WORDS); if(e==NRF_SUCCESS)m_wr=WR_COMPACT_WRITE_WAIT; else if(e!=NRF_ERROR_BUSY){m_wr=WR_ERROR;m_status=NRFCLAW_STATE_ERROR;}
    }
}

nrfclaw_state_status_t nrfclaw_state_status(void){return m_status;}
uint32_t nrfclaw_state_sequence(void){return m_seq;}
