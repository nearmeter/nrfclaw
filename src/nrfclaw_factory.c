#include "nrfclaw_factory.h"
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_nvic.h"
#include "nrf_sdh_soc.h"

/* R3.8.19a layout. Owner key survives factory reset. */
#define FIRST_ADDR 0x0006E000UL
#define FIRST_RANGE_LAST_ADDR 0x00079000UL
#define LORA_PROFILE_FIRST_ADDR 0x0006E000UL
#define LORA_PROFILE_LAST_ADDR  0x0006F000UL

typedef enum { F_IDLE=0,F_ERASE_PENDING,F_ERASE_WAIT,F_RESET_PENDING,F_ERROR } fstate_t;
static fstate_t m_state;
static uint32_t m_addr;
static volatile bool m_ok,m_err;

static void soc_evt(uint32_t e,void*ctx)
{
    (void)ctx;
    if(m_state!=F_ERASE_WAIT)return;
    if(e==NRF_EVT_FLASH_OPERATION_SUCCESS)m_ok=true;
    else if(e==NRF_EVT_FLASH_OPERATION_ERROR)m_err=true;
}
NRF_SDH_SOC_OBSERVER(m_factory_soc_observer,0,soc_evt,NULL);

void nrfclaw_factory_init(void)
{
    m_state=F_IDLE;m_addr=FIRST_ADDR;m_ok=m_err=false;
}

bool nrfclaw_factory_request(void)
{
    if(m_state!=F_IDLE||NRF_FICR->CODEPAGESIZE!=4096U)return false;
    m_addr=FIRST_ADDR;m_ok=m_err=false;m_state=F_ERASE_PENDING;return true;
}

bool nrfclaw_factory_busy(void){return m_state!=F_IDLE;}

static void advance_address(void)
{
    if (m_addr < FIRST_RANGE_LAST_ADDR) {
        m_addr += NRF_FICR->CODEPAGESIZE;
        m_state=F_ERASE_PENDING;
    } else {
        m_state=F_RESET_PENDING;
    }
}

void nrfclaw_factory_process(void)
{
    if(m_err){m_err=false;m_state=F_ERROR;}
    if(m_ok){
        m_ok=false;
        if(m_state==F_ERASE_WAIT)advance_address();
    }
    if(m_state==F_ERASE_PENDING){
        uint32_t e=sd_flash_page_erase(m_addr/NRF_FICR->CODEPAGESIZE);
        if(e==NRF_SUCCESS)m_state=F_ERASE_WAIT;
        else if(e!=NRF_ERROR_BUSY)m_state=F_ERROR;
    } else if(m_state==F_RESET_PENDING) {
        sd_nvic_SystemReset();
    }
}
