#include "nrfclaw_vib_health.h"
#include "nrfclaw_board.h"  /* board_config.h: experimental capability gate */
#include <string.h>

#ifndef NRFCLAW_EXPERIMENTAL_VIB_HEALTH
#define NRFCLAW_EXPERIMENTAL_VIB_HEALTH 0
#endif


/*
 * VIB_HEALTH is board-agnostic. Board profiles opt in with
 * NRFCLAW_EXPERIMENTAL_VIB_HEALTH=1; physical LIS2DH12 presence is checked
 * at runtime by nrfclaw_vib_health_supported().
 */

#define VIB_WARNING_Q8_8 512U
#define VIB_ALARM_Q8_8 1024U

static nrfclaw_vib_model_t m_model;
static bool m_valid;

static uint32_t adiff(uint16_t a,uint16_t b){return a>=b?(uint32_t)(a-b):(uint32_t)(b-a);}
static uint32_t norm(uint16_t v,uint16_t mean,uint16_t tol){
    if(!tol)return 0xFFFFU;
    uint32_t q=(adiff(v,mean)<<8)/tol;
    return q>0xFFFFU?0xFFFFU:q;
}

void nrfclaw_vib_health_init(void){memset(&m_model,0,sizeof(m_model));m_valid=false;}
uint8_t nrfclaw_vib_health_build_gate(void){
#if NRFCLAW_EXPERIMENTAL_VIB_HEALTH
    return 1U;
#else
    return 0U;
#endif
}
bool nrfclaw_vib_health_supported(void){
#if NRFCLAW_EXPERIMENTAL_VIB_HEALTH
    return nrfclaw_lis2dh12_present();
#else
    return false;
#endif
}
bool nrfclaw_vib_health_model_set(nrfclaw_vib_model_t const *m){
#if NRFCLAW_EXPERIMENTAL_VIB_HEALTH
    if(!m||!nrfclaw_lis2dh12_present()||!m->rms_tol_mg||!m->peak_tol_mg||!m->p2p_tol_mg||!m->zero_cross_tol_hz)return false;
    m_model=*m;m_valid=true;return true;
#else
    (void)m;return false;
#endif
}
bool nrfclaw_vib_health_model_get(nrfclaw_vib_model_t *m){if(!m||!m_valid)return false;*m=m_model;return true;}
void nrfclaw_vib_health_model_clear(void){memset(&m_model,0,sizeof(m_model));m_valid=false;}

bool nrfclaw_vib_health_prepare_sample(void){
#if NRFCLAW_EXPERIMENTAL_VIB_HEALTH
    if(!m_valid || !nrfclaw_lis2dh12_present()) return false;

    /* If a completed window is already available there is nothing to start. */
    nrfclaw_accel_vibration_metrics_t ready;
    if(nrfclaw_lis2dh12_vibration_metrics(&ready)) return true;

    /* r1e: service a short FIFO window even when INT2 was not observed. */
    if(nrfclaw_lis2dh12_vibration_service()) return true;

    /* Do not disturb a different accelerometer owner/profile. */
    nrfclaw_accel_mode_t mode=nrfclaw_lis2dh12_mode();
    if(mode==NRFCLAW_ACCEL_MODE_VIBRATION) return true;
    if(mode!=NRFCLAW_ACCEL_MODE_OFF) return false;

    /*
     * One low-duty-cycle health window.  The LIS2DH12 FIFO watermark is 31
     * samples.  At 200 Hz the sensor is active for roughly 155 ms before
     * INT2 causes vibration_capture() to compute the metrics.
     */
    nrfclaw_accel_config_t cfg={
        .mode=NRFCLAW_ACCEL_MODE_VIBRATION,
        .odr_hz=200U,
        .full_scale_g=2U,
        .threshold_mg=0U,
        .duration_ms=0U,
        .low_power=false
    };
    return nrfclaw_lis2dh12_configure(&cfg);
#else
    return false;
#endif
}
bool nrfclaw_vib_health_score(nrfclaw_vib_health_result_t *r){
#if NRFCLAW_EXPERIMENTAL_VIB_HEALTH
    if(!r||!m_valid)return false;
    nrfclaw_accel_vibration_metrics_t m;
    if(!nrfclaw_lis2dh12_vibration_metrics(&m)) {
        (void)nrfclaw_lis2dh12_vibration_service();
        if(!nrfclaw_lis2dh12_vibration_metrics(&m)) return false;
    }
    uint32_t sum=norm(m.rms_mg,m_model.rms_mean_mg,m_model.rms_tol_mg)
        +norm(m.peak_mg,m_model.peak_mean_mg,m_model.peak_tol_mg)
        +norm(m.peak_to_peak_mg,m_model.p2p_mean_mg,m_model.p2p_tol_mg)
        +norm(m.zero_cross_hz,m_model.zero_cross_mean_hz,m_model.zero_cross_tol_hz);
    uint32_t score=sum/4U;if(score>0xFFFFU)score=0xFFFFU;
    memset(r,0,sizeof(*r));r->model_valid=true;r->score_q8_8=(uint16_t)score;r->metrics=m;
    r->state=score>=VIB_ALARM_Q8_8?NRFCLAW_VIB_HEALTH_ALARM:(score>=VIB_WARNING_Q8_8?NRFCLAW_VIB_HEALTH_WARNING:NRFCLAW_VIB_HEALTH_NORMAL);

    /* Health windows are intentionally one-shot.  Once the result has been
     * copied out, return the LIS2DH12 to power-down immediately. */
    nrfclaw_lis2dh12_disable();
    return true;
#else
    (void)r;return false;
#endif
}
