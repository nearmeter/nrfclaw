#include "nrfclaw_rf_ingest.h"

#include "nrfclaw_board.h"
#include "nrfclaw_llcc68_rl.h"
#include "nrfclaw_rfvm.h"
#include "nrfclaw_rf_capability.h"
#include "nrfclaw_rf_ninalink.h"
#include "nrfclaw_rtc.h"

#include "nrf_drv_gpiote.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static nrfclaw_rf_ingest_status_t m_status;
static uint8_t m_last_frame[64];
static uint8_t m_last_frame_len;
static uint8_t m_last_frame_profile;
static bool m_last_frame_valid;
static bool m_last_frame_rfvm_ok;

static uint16_t sat_inc16(uint16_t v)
{
    return v == UINT16_MAX ? v : (uint16_t)(v + 1U);
}

static void dio1_enable(void)
{
#if NRFCLAW_BOARD_HAS_LORA
    nrf_drv_gpiote_in_event_enable(P_LORA_DIO1, true);
#endif
}

static void dio1_disable(void)
{
#if NRFCLAW_BOARD_HAS_LORA
    nrf_drv_gpiote_in_event_disable(P_LORA_DIO1);
#endif
}

bool nrfclaw_rf_ingest_start(uint8_t profile_id)
{
    if (!nrfclaw_llcc68_rl_gfsk_active())
        return false;
    if (!nrfclaw_rfvm_profile(profile_id))
        return false;

    memset(&m_status, 0, sizeof(m_status));
    m_status.active = true;
    m_status.profile_id = profile_id;

    /* main.c ISR disables DIO1 before queueing NRFCLAW_EVT_LORA_DIO1. */
    dio1_enable();
    return true;
}

void nrfclaw_rf_ingest_stop(void)
{
    m_status.active = false;
    dio1_disable();
}

bool nrfclaw_rf_ingest_active(void)
{
    return m_status.active;
}

void nrfclaw_rf_ingest_clear_stats(void)
{
    bool active = m_status.active;
    uint8_t profile_id = m_status.profile_id;
    memset(&m_status, 0, sizeof(m_status));
    m_status.active = active;
    m_status.profile_id = profile_id;
}

void nrfclaw_rf_ingest_get_status(nrfclaw_rf_ingest_status_t *out)
{
    if (out)
        *out = m_status;
}

bool nrfclaw_rf_ingest_last_frame_meta(
    uint8_t *profile_id, uint8_t *length, bool *rfvm_ok)
{
    if(!m_last_frame_valid) return false;
    if(profile_id) *profile_id=m_last_frame_profile;
    if(length) *length=m_last_frame_len;
    if(rfvm_ok) *rfvm_ok=m_last_frame_rfvm_ok;
    return true;
}

uint8_t nrfclaw_rf_ingest_last_frame_read(
    uint8_t offset, uint8_t *out, uint8_t max_len)
{
    uint8_t n;
    if(!m_last_frame_valid || !out || max_len==0U || offset>=m_last_frame_len) return 0U;
    n=(uint8_t)(m_last_frame_len-offset);
    if(n>max_len) n=max_len;
    memcpy(out,&m_last_frame[offset],n);
    return n;
}

bool nrfclaw_rf_ingest_on_dio1(void)
{
    static uint8_t frame[256];
    size_t len = 0U;
    uint16_t irq = 0U;
    bool ready = false;
    nrfclaw_rfvm_result_t decoded;
    nrfclaw_rfvm_profile_t const *profile;

    if (!m_status.active)
        return false;

    m_status.dio1_events = sat_inc16(m_status.dio1_events);

    if (!nrfclaw_llcc68_rl_gfsk_poll_ex(
            &irq, frame, &len, sizeof(frame), &ready)) {
        m_status.failures = sat_inc16(m_status.failures);
        dio1_enable();
        return true;
    }

    m_status.last_irq = irq;

    if (!ready) {
        dio1_enable();
        return true;
    }

    m_status.packets = sat_inc16(m_status.packets);
    m_status.last_len=(uint8_t)(len>255U?255U:len);

    /* B8.0m3a exact FIFO trace. */
    if(len <= sizeof(m_last_frame)){
        memcpy(m_last_frame,frame,len);
        m_last_frame_len=(uint8_t)len;
        m_last_frame_profile=m_status.profile_id;
        m_last_frame_valid=true;
        m_last_frame_rfvm_ok=false;
    } else {
        /* B8.1k6c invalidate short-frame trace. */
        m_last_frame_valid=false;
    }

    profile = nrfclaw_rfvm_profile(m_status.profile_id);
    if (!profile || !nrfclaw_rfvm_run(profile, frame, len, &decoded)) {
        m_status.failures = sat_inc16(m_status.failures);
        dio1_enable();
        return true;
    }

    m_status.decode_ok = sat_inc16(m_status.decode_ok);
    m_last_frame_rfvm_ok=true;

    if (!nrfclaw_rf_capability_publish(profile->profile_id, &decoded)) {
        m_status.failures = sat_inc16(m_status.failures);
        dio1_enable();
        return true;
    }

    if (!nrfclaw_rf_ninalink_publish(
            profile->profile_id,
            nrfclaw_rtc_now())) {
        m_status.failures = sat_inc16(m_status.failures);
        dio1_enable();
        return true;
    }

    m_status.publish_ok = sat_inc16(m_status.publish_ok);

    /* gfsk_poll() already cleared RX_DONE and re-entered continuous RX. */
    dio1_enable();
    return true;
}
