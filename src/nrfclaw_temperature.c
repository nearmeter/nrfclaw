#include "nrfclaw_temperature.h"

#include "nrf_error.h"
#include "nrf_soc.h"
#include "nrfclaw_ds18b20.h"

static nrfclaw_temperature_source_t m_source;
static bool m_valid;
static int32_t m_last_mC;

static bool publish_internal_sample(void)
{
    int32_t quarter_c = 0;
    nrfclaw_event_t done;

    if (sd_temp_get(&quarter_c) != NRF_SUCCESS)
        return false;

    m_source = NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
    m_last_mC = quarter_c * 250L;
    m_valid = true;

    done.type = NRFCLAW_EVT_TEMPERATURE_DONE;
    done.arg0 = (uint32_t)m_last_mC;
    done.arg1 = (uint32_t)m_source;
    return nrfclaw_event_push(&done);
}

void nrfclaw_temperature_init(void)
{
    m_source = nrfclaw_ds18b20_present()
        ? NRFCLAW_TEMP_SOURCE_DS18B20
        : NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
    m_valid = false;
    m_last_mC = 0;
}

bool nrfclaw_temperature_available(void)
{
    return true;
}

nrfclaw_temperature_source_t nrfclaw_temperature_source(void)
{
    if (m_source == NRFCLAW_TEMP_SOURCE_DS18B20 &&
        !nrfclaw_ds18b20_present()) {
        m_source = NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
        m_valid = false;
    }
    return m_source;
}

bool nrfclaw_temperature_busy(void)
{
    return nrfclaw_temperature_source() == NRFCLAW_TEMP_SOURCE_DS18B20 &&
           nrfclaw_ds18b20_busy();
}

bool nrfclaw_temperature_start(void)
{
    if (nrfclaw_ds18b20_present()) {
        m_source = NRFCLAW_TEMP_SOURCE_DS18B20;
        m_valid = false;
        if (nrfclaw_ds18b20_busy())
            return false;
        if (nrfclaw_ds18b20_start())
            return true;
    }

    m_source = NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL;
    m_valid = false;
    return publish_internal_sample();
}

bool nrfclaw_temperature_last_mC(int32_t *milli_c)
{
    if (!milli_c)
        return false;

    if (nrfclaw_temperature_source() == NRFCLAW_TEMP_SOURCE_DS18B20) {
        int32_t mc;
        if (!nrfclaw_ds18b20_last_mC(&mc))
            return false;
        m_last_mC = mc;
        m_valid = true;
    }

    if (!m_valid)
        return false;

    *milli_c = m_last_mC;
    return true;
}

void nrfclaw_temperature_on_event(nrfclaw_event_t const *evt)
{
    nrfclaw_event_t done;
    if (!evt || evt->type != NRFCLAW_EVT_DS18B20_DONE)
        return;

    m_source = NRFCLAW_TEMP_SOURCE_DS18B20;
    m_last_mC = (int32_t)evt->arg0;
    m_valid = true;

    done.type = NRFCLAW_EVT_TEMPERATURE_DONE;
    done.arg0 = evt->arg0;
    done.arg1 = (uint32_t)m_source;
    (void)nrfclaw_event_push(&done);
}

void nrfclaw_temperature_idle_lowpower(void)
{
    nrfclaw_ds18b20_idle_lowpower();
}
