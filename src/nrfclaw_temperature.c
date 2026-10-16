#include "nrfclaw_temperature.h"
#include "nrf_error.h"
#include "nrf_soc.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_sht.h"

static nrfclaw_temperature_source_t m_source;
static bool m_valid;
static int32_t m_last_mC;
static bool m_humidity_valid;
static uint32_t m_last_humidity_mpermille;
static int32_t m_last_sht_temperature_mC;

static bool probe_default_sht(void)
{
    nrfclaw_sht_status_t st;
    if(nrfclaw_sht_present()) return true;
    if(nrfclaw_sht_active()) nrfclaw_sht_release();
    st=nrfclaw_sht_configure_default();
    if(st!=NRFCLAW_SHT_OK) return false;
    st=nrfclaw_sht_probe();
    if(st!=NRFCLAW_SHT_OK){ nrfclaw_sht_release(); return false; }
    return true;
}

static bool refresh_sht_sample(void)
{
    nrfclaw_sht_sample_t s;
    if(!nrfclaw_sht_present()){ m_humidity_valid=false; return false; }
    if(nrfclaw_sht_read(&s)!=NRFCLAW_SHT_OK){ m_humidity_valid=false; return false; }
    m_last_sht_temperature_mC=s.temperature_mC;
    m_last_humidity_mpermille=s.humidity_mpermille;
    m_humidity_valid=true;
    if(m_source==NRFCLAW_TEMP_SOURCE_SHT){ m_last_mC=s.temperature_mC; m_valid=true; }
    return true;
}

static bool publish_done(nrfclaw_temperature_source_t source,int32_t mc)
{
    nrfclaw_event_t e;
    m_source=source; m_last_mC=mc; m_valid=true;
    e.type=NRFCLAW_EVT_TEMPERATURE_DONE; e.arg0=(uint32_t)mc; e.arg1=(uint32_t)source;
    return nrfclaw_event_push(&e);
}

static bool publish_internal_sample(void)
{
    int32_t quarter_c=0;
    if(sd_temp_get(&quarter_c)!=NRF_SUCCESS) return false;
    return publish_done(NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL,quarter_c*250L);
}

void nrfclaw_temperature_init(void)
{
    m_valid=false; m_last_mC=0; m_humidity_valid=false;
    m_last_humidity_mpermille=0U; m_last_sht_temperature_mC=0;
    (void)probe_default_sht();
    /* B8.1k8c2 frozen preference: DS18B20 -> SHT -> nRF52 internal. */
    if(nrfclaw_ds18b20_present()) m_source=NRFCLAW_TEMP_SOURCE_DS18B20;
    else if(nrfclaw_sht_present()) m_source=NRFCLAW_TEMP_SOURCE_SHT;
    else m_source=NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
}

bool nrfclaw_temperature_available(void){ return true; }

nrfclaw_temperature_source_t nrfclaw_temperature_source(void)
{
    if(m_source==NRFCLAW_TEMP_SOURCE_DS18B20 && !nrfclaw_ds18b20_present()){
        m_source=nrfclaw_sht_present()?NRFCLAW_TEMP_SOURCE_SHT:NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
        m_valid=false;
    }
    if(m_source==NRFCLAW_TEMP_SOURCE_SHT && !nrfclaw_sht_present()){
        m_source=NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL; m_valid=false; m_humidity_valid=false;
    }
    return m_source;
}

bool nrfclaw_temperature_busy(void)
{
    return nrfclaw_temperature_source()==NRFCLAW_TEMP_SOURCE_DS18B20 && nrfclaw_ds18b20_busy();
}

bool nrfclaw_temperature_start(void)
{
    if(nrfclaw_ds18b20_present()){
        /* Refresh SHT in the same periodic acquisition for HUMIDITY, but DS18 stays authoritative for T. */
        if(nrfclaw_sht_present()) (void)refresh_sht_sample();
        m_source=NRFCLAW_TEMP_SOURCE_DS18B20; m_valid=false;
        if(nrfclaw_ds18b20_busy()) return false;
        if(nrfclaw_ds18b20_start()) return true;
    }
    if(nrfclaw_sht_present()){
        m_source=NRFCLAW_TEMP_SOURCE_SHT; m_valid=false;
        if(refresh_sht_sample()) return publish_done(NRFCLAW_TEMP_SOURCE_SHT,m_last_sht_temperature_mC);
    }
    m_source=NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL; m_valid=false;
    return publish_internal_sample();
}

bool nrfclaw_temperature_last_mC(int32_t *mc)
{
    if(!mc) return false;
    if(nrfclaw_temperature_source()==NRFCLAW_TEMP_SOURCE_DS18B20){
        int32_t v; if(!nrfclaw_ds18b20_last_mC(&v)) return false;
        m_last_mC=v; m_valid=true;
    }
    if(!m_valid) return false;
    *mc=m_last_mC; return true;
}

bool nrfclaw_humidity_available(void){ return nrfclaw_sht_present(); }
bool nrfclaw_humidity_refresh(void){ return refresh_sht_sample(); }
bool nrfclaw_humidity_last_mpermille(uint32_t *rh)
{
    if(!rh || !nrfclaw_sht_present() || !m_humidity_valid) return false;
    *rh=m_last_humidity_mpermille; return true;
}

void nrfclaw_temperature_on_event(nrfclaw_event_t const *evt)
{
    nrfclaw_event_t done;
    if(!evt || evt->type!=NRFCLAW_EVT_DS18B20_DONE) return;
    m_source=NRFCLAW_TEMP_SOURCE_DS18B20; m_last_mC=(int32_t)evt->arg0; m_valid=true;
    done.type=NRFCLAW_EVT_TEMPERATURE_DONE; done.arg0=evt->arg0; done.arg1=(uint32_t)m_source;
    (void)nrfclaw_event_push(&done);
}
void nrfclaw_temperature_idle_lowpower(void){ nrfclaw_ds18b20_idle_lowpower(); }
