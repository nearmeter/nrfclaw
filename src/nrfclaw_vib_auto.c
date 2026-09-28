#include "nrfclaw_vib_auto.h"
#include "nrfclaw_vib_auto_store.h"
#include "nrfclaw_board.h"
#include "nrfclaw_rtc.h"
#include "app_timer.h"
#include "app_error.h"
#include <string.h>

#ifndef NRFCLAW_EXPERIMENTAL_VIB_AUTO
#define NRFCLAW_EXPERIMENTAL_VIB_AUTO 0
#endif

#ifndef NRFCLAW_EXPERIMENTAL_VIB_HEALTH
#define NRFCLAW_EXPERIMENTAL_VIB_HEALTH 0
#endif

/*
 * VIB_AUTO is a hardware capability, not a NINASENSE-only feature.
 * A board profile opts in with NRFCLAW_EXPERIMENTAL_VIB_AUTO=1; runtime
 * support is then validated by nrfclaw_vib_auto_supported(), which requires
 * the LIS2DH12 to be physically present and VIB_HEALTH to be supported.
 *
 * Keep the dependency explicit: the autonomous classifier is implemented on
 * top of the vibration-health metrics and must not be built without them.
 */
#if NRFCLAW_EXPERIMENTAL_VIB_AUTO && !NRFCLAW_EXPERIMENTAL_VIB_HEALTH
#error "NRFCLAW_EXPERIMENTAL_VIB_AUTO=1 requires NRFCLAW_EXPERIMENTAL_VIB_HEALTH=1"
#endif

#if NRFCLAW_EXPERIMENTAL_VIB_AUTO
#define VIB_AUTO_COMPILED 1
#else
#define VIB_AUTO_COMPILED 0
#endif

#define VA_SAMPLE_ODR_HZ              200U
#define VA_TIMER_CHUNK_S              240U
#define VA_WARNING_NORMAL_Q8_8        512U   /* 2.0 */
#define VA_ALARM_NORMAL_Q8_8          1024U  /* 4.0 */
#define VA_MIN_RMS_TOL_MG             10U
#define VA_MIN_PEAK_TOL_MG            20U
#define VA_MIN_P2P_TOL_MG             30U
#define VA_MIN_ZC_TOL_HZ              2U
#define VA_DISCOVERY_DEV_MULT          4U
#define VA_MONITOR_DEV_MULT            2U
#define VA_MONITOR_REL_PCT             75U
#define VA_PERSIST_MIN_INTERVAL_S      21600UL
#define VA_PERSIST_ADAPT_UPDATES       64U
#define VA_ALARM_RECOVERY_GOOD         3U
#define VA_ACTIVE_CONFIRM_DELAY_S       2U   /* first continuity check after INT1-active FIFO */
#define VA_ACTIVE_CONFIRM_RETRY_MS       500U /* one short second chance for a weak/phase window */
#define VA_ACTIVE_CONFIRM_MAX_RETRIES    1U   /* max extra FIFO windows after the first confirm */

APP_TIMER_DEF(m_va_timer);

typedef enum { PURPOSE_NONE=0, PURPOSE_DISCOVERY, PURPOSE_MONITOR, PURPOSE_ARMING } sample_purpose_t;

typedef struct {
    nrfclaw_vib_auto_profile_t pub;
    uint16_t dev_rms,dev_peak,dev_p2p,dev_zc;
    int32_t residual_rms,residual_peak,residual_p2p,residual_zc;
} runtime_profile_t;

typedef struct {
    bool valid;
    uint8_t count;
    uint16_t rms,peak,p2p,zc;
    uint16_t dev_rms,dev_peak,dev_p2p,dev_zc;
} candidate_t;

static nrfclaw_vib_auto_config_t m_cfg;
static nrfclaw_vib_auto_status_t m_status;
static runtime_profile_t m_profiles[NRFCLAW_VIB_AUTO_MAX_PROFILES];
static uint8_t m_profile_count;
static candidate_t m_candidate;
static uint16_t m_quiet_rms,m_quiet_peak,m_quiet_count;
static uint32_t m_discovery_started_at;
static bool m_discovery_started;
/* r3.8.7: learning deadline expired without a promotable profile.
 * This is a latched runtime state, intentionally not persisted: while set,
 * WAIT_MACHINE remains INT1-only and maybe_finish_discovery() must not
 * repeatedly re-arm the LIS2DH12 from the main loop. */
static bool m_discovery_deadline_wait;
static bool m_timer_due;
static uint32_t m_delay_remaining_s;
static sample_purpose_t m_sample_purpose;
static bool m_sampling;
static uint8_t m_alarm_recovery_good;
static uint32_t m_prng;
static uint16_t m_adapt_updates_since_save;
static uint32_t m_last_persist_s;

/* r3.8.10: RAM-only transition trace; survives until reset/relearn. */
static nrfclaw_vib_auto_trace_entry_t m_trace[NRFCLAW_VIB_AUTO_TRACE_LEN];
static uint8_t m_trace_head;
static uint8_t m_trace_count;
static uint8_t m_last_arm_reason;
/* r3.8.12: an INT1 wake is only a candidate machine start.  The first
 * active FIFO arms one short confirmation window.  Only a second active
 * window is admitted to discovery/monitoring. */
static bool m_need_active_confirm;
static bool m_active_confirm_pending;
static uint8_t m_active_confirm_retry_count;

enum {
    TRACE_ARMING_DONE=1, TRACE_ARM_WAKE=2, TRACE_TIMER_SCHEDULE=3, TRACE_TIMER_DUE=4,
    TRACE_MOTION_ACCEPT=5, TRACE_MOTION_IGNORED=6, TRACE_INT_DISABLE=7, TRACE_FIFO_BEGIN=8,
    TRACE_FIFO_CONFIG_OK=9, TRACE_METRICS_ACTIVE=10, TRACE_METRICS_QUIET=11, TRACE_DISCOVERY_SCHEDULE=12,
    TRACE_FIFO_WATCHDOG_FAIL=13, TRACE_CONFIRM_SCHEDULE=14, TRACE_CONFIRM_OK=15, TRACE_CONFIRM_REJECT=16, TRACE_CONFIRM_RETRY=17, TRACE_CONFIRM_RETRY_OK=18
};

static void trace_add(uint8_t event,uint8_t arm_reason){
    nrfclaw_vib_auto_trace_entry_t *e=&m_trace[m_trace_head];
    memset(e,0,sizeof(*e));
    e->timestamp_s=nrfclaw_rtc_now(); e->event=event; e->state=(uint8_t)m_status.state;
    e->purpose=(uint8_t)m_sample_purpose; e->accel_mode=(uint8_t)nrfclaw_lis2dh12_mode(); e->arm_reason=arm_reason;
    m_trace_head=(uint8_t)((m_trace_head+1U)%NRFCLAW_VIB_AUTO_TRACE_LEN);
    if(m_trace_count<NRFCLAW_VIB_AUTO_TRACE_LEN)m_trace_count++;
}


static uint16_t clamp16u32(uint32_t v){return (uint16_t)(v>65535U?65535U:v);}
static uint16_t uabs16diff(uint16_t a,uint16_t b){return a>b?(uint16_t)(a-b):(uint16_t)(b-a);}
static int32_t sdelta(uint16_t sample,uint16_t mean){return (int32_t)sample-(int32_t)mean;}
static uint16_t uabs32_to16(int32_t x){uint32_t u=(uint32_t)(x<0?-x:x);return clamp16u32(u);}
static uint16_t max16(uint16_t a,uint16_t b){return a>b?a:b;}
static void sat_inc16(uint16_t *v){if(v&&*v<65535U)(*v)++;}

static void diagnostics_reset(void){
    m_status.diag_total_windows=0U;m_status.diag_quiet_windows=0U;m_status.diag_active_windows=0U;
    m_status.diag_candidate_starts=0U;m_status.diag_candidate_matches=0U;m_status.diag_candidate_resets=0U;m_status.diag_profile_matches=0U;
    m_status.diag_reject_rms=0U;m_status.diag_reject_peak=0U;m_status.diag_reject_p2p=0U;m_status.diag_reject_zc=0U;
    m_status.diag_last_candidate_score_q8_8=0U;memset(m_status.diag_last_feature_score_q8_8,0,sizeof(m_status.diag_last_feature_score_q8_8));
    m_status.diag_last_reject_feature=0U;m_status.diag_last_reject_mask=0U;
    m_status.diag_timer_fires=0U;m_status.diag_discovery_due=0U;m_status.diag_probe_requests=0U;m_status.diag_motion_releases=0U;
    m_status.diag_window_config_ok=0U;m_status.diag_window_config_fail=0U;m_status.diag_metrics_ready=0U;m_status.diag_probe_stage=0U;m_status.diag_probe_started_at=0U;
    m_status.diag_schedule_calls=0U;m_status.diag_timer_stop_calls=0U;m_status.diag_timer_start_ok=0U;m_status.diag_timer_start_fail=0U;
    m_status.diag_motion_events=0U;m_status.diag_motion_accepted=0U;m_status.diag_motion_ignored_state=0U;
    m_status.diag_arm_wake_calls=0U;m_status.diag_arm_wake_ok=0U;m_status.diag_arm_wake_fail=0U;
    m_status.diag_last_schedule_s=0U;m_status.diag_last_schedule_purpose=0U;m_status.diag_last_trigger=0U;
    m_status.diag_arming_starts=0U;m_status.diag_arming_completes=0U;m_status.diag_motion_ignored_arming=0U;m_status.diag_int_first_windows=0U;
    m_status.diag_fifo_watchdog_failures=0U;
    m_status.diag_confirm_starts=0U;m_status.diag_confirm_ok=0U;m_status.diag_confirm_rejects=0U;
    m_status.diag_confirm_retries=0U;m_status.diag_confirm_retry_ok=0U;
    m_status.diag_confirm_delay_s=VA_ACTIVE_CONFIRM_DELAY_S;m_status.diag_confirm_retry_delay_ms=VA_ACTIVE_CONFIRM_RETRY_MS;m_status.diag_confirm_pending=false;
    memset(m_trace,0,sizeof(m_trace));m_trace_head=0U;m_trace_count=0U;m_last_arm_reason=0U;m_need_active_confirm=false;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;
}

static void emit(nrfclaw_event_type_t type,uint32_t a0,uint32_t a1){nrfclaw_event_t e={.type=type,.arg0=a0,.arg1=a1};(void)nrfclaw_event_push(&e);}

static void default_config(void){
    memset(&m_cfg,0,sizeof(m_cfg));
    m_cfg.learning_time_s=3600UL;
    m_cfg.discovery_interval_s=60U;
    m_cfg.normal_interval_s=600U;
    m_cfg.suspicious_interval_s=10U;
    m_cfg.wake_threshold_mg=120U;
    m_cfg.wake_duration_ms=100U;
    m_cfg.off_rms_mg=5U;
    m_cfg.profile_match_q8_8=640U; /* 2.5 */
    m_cfg.adapt_limit_q8_8=256U;  /* 1.0 */
    m_cfg.confirm_delay_s=60U; /* legacy mirror for older NDP clients */
    m_cfg.arming_delay_s=60UL;
    m_cfg.candidate_confirmations=3U;
    m_cfg.alarm_consecutive=3U;
    m_cfg.max_profiles=NRFCLAW_VIB_AUTO_MAX_PROFILES;
    m_cfg.sensitivity=1U;
    m_cfg.adaptation_shift=11U;
    m_cfg.stable_expand_after=8U;
    m_cfg.max_interval_multiplier=4U;
}

static bool config_valid(nrfclaw_vib_auto_config_t const *c){
    if(!c)return false;
    if(c->learning_time_s<60UL||c->learning_time_s>604800UL)return false;
    if(c->discovery_interval_s<1U||c->normal_interval_s<1U||c->suspicious_interval_s<1U)return false;
    if(c->wake_threshold_mg<16U||c->wake_threshold_mg>2000U)return false;
    if(c->wake_duration_ms>5000U)return false;
    if(c->arming_delay_s>604800UL)return false; /* installation delay: 0..7 days */
    if(c->off_rms_mg>1000U)return false;
    if(c->profile_match_q8_8<128U||c->profile_match_q8_8>2048U)return false;
    if(c->adapt_limit_q8_8>c->profile_match_q8_8)return false;
    if(c->candidate_confirmations<2U||c->candidate_confirmations>20U)return false;
    if(c->alarm_consecutive<1U||c->alarm_consecutive>10U)return false;
    if(c->max_profiles<1U||c->max_profiles>NRFCLAW_VIB_AUTO_MAX_PROFILES)return false;
    if(c->sensitivity>2U)return false;
    if(c->adaptation_shift<8U||c->adaptation_shift>15U)return false;
    if(c->stable_expand_after<2U)return false;
    if(c->max_interval_multiplier<1U||c->max_interval_multiplier>8U)return false;
    return true;
}

static uint16_t warning_threshold(void){
    if(m_cfg.sensitivity==2U)return 384U; /* 1.5 */
    if(m_cfg.sensitivity==0U)return 640U; /* 2.5 */
    return VA_WARNING_NORMAL_Q8_8;
}
static uint16_t alarm_threshold(void){
    if(m_cfg.sensitivity==2U)return 768U;  /* 3.0 */
    if(m_cfg.sensitivity==0U)return 1280U; /* 5.0 */
    return VA_ALARM_NORMAL_Q8_8;
}

static uint16_t discovery_tol_from_dev(uint16_t dev,uint16_t floorv){
    uint32_t t=(uint32_t)dev*VA_DISCOVERY_DEV_MULT;
    uint32_t minv=(uint32_t)floorv*2U;
    if(t<minv)t=minv;
    return clamp16u32(t);
}

static uint16_t monitor_tol_from_dev(uint16_t dev,uint16_t mean,uint16_t floorv){
    uint32_t t=(uint32_t)dev*VA_MONITOR_DEV_MULT;
    if(t<floorv)t=floorv;
    /* Bound mature monitoring profiles so a few high-variance discovery
     * windows cannot make a profile accept nearly everything.  The floor is
     * added to retain useful headroom for low-amplitude machines. */
    uint32_t cap=((uint32_t)mean*VA_MONITOR_REL_PCT)/100U+(uint32_t)floorv;
    if(cap<floorv)cap=floorv;
    if(t>cap)t=cap;
    return clamp16u32(t);
}

static uint16_t feature_score(uint16_t v,uint16_t mean,uint16_t tol){
    if(tol==0U)tol=1U;
    uint32_t q=((uint32_t)uabs16diff(v,mean)<<8)/tol;
    return clamp16u32(q);
}

static uint16_t profile_score_values(nrfclaw_vib_auto_profile_t const *p,uint16_t rms,uint16_t peak,uint16_t p2p,uint16_t zc){
    uint32_t s=0;
    s+=feature_score(rms,p->rms_mean_mg,p->rms_tol_mg);
    s+=feature_score(peak,p->peak_mean_mg,p->peak_tol_mg);
    s+=feature_score(p2p,p->p2p_mean_mg,p->p2p_tol_mg);
    s+=feature_score(zc,p->zero_cross_mean_hz,p->zero_cross_tol_hz);
    return clamp16u32(s/4U);
}

static uint16_t profile_score(runtime_profile_t const *p,nrfclaw_accel_vibration_metrics_t const *m){
    return profile_score_values(&p->pub,m->rms_mg,m->peak_mg,m->peak_to_peak_mg,m->zero_cross_hz);
}

static uint16_t discovery_profile_score(runtime_profile_t const *p,nrfclaw_accel_vibration_metrics_t const *m){
    nrfclaw_vib_auto_profile_t d=p->pub;
    d.rms_tol_mg=discovery_tol_from_dev(p->dev_rms,VA_MIN_RMS_TOL_MG);
    d.peak_tol_mg=discovery_tol_from_dev(p->dev_peak,VA_MIN_PEAK_TOL_MG);
    d.p2p_tol_mg=discovery_tol_from_dev(p->dev_p2p,VA_MIN_P2P_TOL_MG);
    d.zero_cross_tol_hz=discovery_tol_from_dev(p->dev_zc,VA_MIN_ZC_TOL_HZ);
    return profile_score_values(&d,m->rms_mg,m->peak_mg,m->peak_to_peak_mg,m->zero_cross_hz);
}

static void update_confidence(runtime_profile_t *p){
    uint32_t obs=p->pub.observations;
    uint32_t c=(obs>=50U)?100U:(obs*2U);
    p->pub.confidence_pct=(uint8_t)c;
}

static void profile_init(runtime_profile_t *p,uint16_t rms,uint16_t peak,uint16_t p2p,uint16_t zc,uint8_t observations,
                         uint16_t dr,uint16_t dp,uint16_t dpp,uint16_t dz){
    memset(p,0,sizeof(*p));
    p->pub.rms_mean_mg=rms;p->pub.peak_mean_mg=peak;p->pub.p2p_mean_mg=p2p;p->pub.zero_cross_mean_hz=zc;
    p->dev_rms=dr;p->dev_peak=dp;p->dev_p2p=dpp;p->dev_zc=dz;
    p->pub.rms_tol_mg=monitor_tol_from_dev(dr,rms,VA_MIN_RMS_TOL_MG);
    p->pub.peak_tol_mg=monitor_tol_from_dev(dp,peak,VA_MIN_PEAK_TOL_MG);
    p->pub.p2p_tol_mg=monitor_tol_from_dev(dpp,p2p,VA_MIN_P2P_TOL_MG);
    p->pub.zero_cross_tol_hz=monitor_tol_from_dev(dz,zc,VA_MIN_ZC_TOL_HZ);
    p->pub.observations=observations;update_confidence(p);
}

static uint16_t incremental_mean(uint16_t mean,uint16_t v,uint16_t n){
    int32_t d=(int32_t)v-(int32_t)mean;
    int32_t out=(int32_t)mean+d/(int32_t)n;
    if(out<0)out=0;
    if(out>65535)out=65535;
    return (uint16_t)out;
}
static uint16_t incremental_dev(uint16_t dev,uint16_t absdev,uint16_t n){
    int32_t d=(int32_t)absdev-(int32_t)dev;
    int32_t out=(int32_t)dev+d/(int32_t)n;
    if(out<0)out=0;
    if(out>65535)out=65535;
    return (uint16_t)out;
}

static void profile_discovery_update(runtime_profile_t *p,nrfclaw_accel_vibration_metrics_t const *m){
    uint16_t n=p->pub.observations<65534U?(uint16_t)(p->pub.observations+1U):65535U;
    uint16_t old_r=p->pub.rms_mean_mg,old_p=p->pub.peak_mean_mg,old_pp=p->pub.p2p_mean_mg,old_z=p->pub.zero_cross_mean_hz;
    p->pub.rms_mean_mg=incremental_mean(old_r,m->rms_mg,n);
    p->pub.peak_mean_mg=incremental_mean(old_p,m->peak_mg,n);
    p->pub.p2p_mean_mg=incremental_mean(old_pp,m->peak_to_peak_mg,n);
    p->pub.zero_cross_mean_hz=incremental_mean(old_z,m->zero_cross_hz,n);
    p->dev_rms=incremental_dev(p->dev_rms,uabs16diff(m->rms_mg,p->pub.rms_mean_mg),n);
    p->dev_peak=incremental_dev(p->dev_peak,uabs16diff(m->peak_mg,p->pub.peak_mean_mg),n);
    p->dev_p2p=incremental_dev(p->dev_p2p,uabs16diff(m->peak_to_peak_mg,p->pub.p2p_mean_mg),n);
    p->dev_zc=incremental_dev(p->dev_zc,uabs16diff(m->zero_cross_hz,p->pub.zero_cross_mean_hz),n);
    p->pub.rms_tol_mg=monitor_tol_from_dev(p->dev_rms,p->pub.rms_mean_mg,VA_MIN_RMS_TOL_MG);
    p->pub.peak_tol_mg=monitor_tol_from_dev(p->dev_peak,p->pub.peak_mean_mg,VA_MIN_PEAK_TOL_MG);
    p->pub.p2p_tol_mg=monitor_tol_from_dev(p->dev_p2p,p->pub.p2p_mean_mg,VA_MIN_P2P_TOL_MG);
    p->pub.zero_cross_tol_hz=monitor_tol_from_dev(p->dev_zc,p->pub.zero_cross_mean_hz,VA_MIN_ZC_TOL_HZ);
    p->pub.observations=n;update_confidence(p);
}

static void candidate_clear(void){memset(&m_candidate,0,sizeof(m_candidate));m_status.candidate_count=0U;}
static void candidate_init(nrfclaw_accel_vibration_metrics_t const *m){
    candidate_clear();m_candidate.valid=true;m_candidate.count=1U;sat_inc16(&m_status.diag_candidate_starts);
    m_candidate.rms=m->rms_mg;m_candidate.peak=m->peak_mg;m_candidate.p2p=m->peak_to_peak_mg;m_candidate.zc=m->zero_cross_hz;
    m_candidate.dev_rms=VA_MIN_RMS_TOL_MG/4U;m_candidate.dev_peak=VA_MIN_PEAK_TOL_MG/4U;
    m_candidate.dev_p2p=VA_MIN_P2P_TOL_MG/4U;m_candidate.dev_zc=1U;m_status.candidate_count=1U;
}
static uint16_t candidate_score_detail(nrfclaw_accel_vibration_metrics_t const *m,uint16_t fs[4]){
    uint16_t tr=max16(discovery_tol_from_dev(m_candidate.dev_rms,VA_MIN_RMS_TOL_MG),(uint16_t)(VA_MIN_RMS_TOL_MG*2U));
    uint16_t tp=max16(discovery_tol_from_dev(m_candidate.dev_peak,VA_MIN_PEAK_TOL_MG),(uint16_t)(VA_MIN_PEAK_TOL_MG*2U));
    uint16_t tpp=max16(discovery_tol_from_dev(m_candidate.dev_p2p,VA_MIN_P2P_TOL_MG),(uint16_t)(VA_MIN_P2P_TOL_MG*2U));
    uint16_t tz=max16(discovery_tol_from_dev(m_candidate.dev_zc,VA_MIN_ZC_TOL_HZ),(uint16_t)(VA_MIN_ZC_TOL_HZ*2U));
    fs[0]=feature_score(m->rms_mg,m_candidate.rms,tr);
    fs[1]=feature_score(m->peak_mg,m_candidate.peak,tp);
    fs[2]=feature_score(m->peak_to_peak_mg,m_candidate.p2p,tpp);
    fs[3]=feature_score(m->zero_cross_hz,m_candidate.zc,tz);
    uint32_t sum=(uint32_t)fs[0]+fs[1]+fs[2]+fs[3];
    return clamp16u32(sum/4U);
}
static void candidate_update(nrfclaw_accel_vibration_metrics_t const *m){
    if(!m_candidate.valid){candidate_init(m);return;}
    uint16_t fs[4];uint16_t score=candidate_score_detail(m,fs);uint16_t limit=(uint16_t)(m_cfg.profile_match_q8_8+256U);
    m_status.diag_last_candidate_score_q8_8=score;
    memcpy(m_status.diag_last_feature_score_q8_8,fs,sizeof(fs));
    if(score>limit){
        sat_inc16(&m_status.diag_candidate_resets);
        uint8_t dominant=0U;for(uint8_t i=1U;i<4U;i++)if(fs[i]>fs[dominant])dominant=i;
        m_status.diag_last_reject_feature=(uint8_t)(dominant+1U);m_status.diag_last_reject_mask=0U;
        for(uint8_t i=0U;i<4U;i++)if(fs[i]>limit)m_status.diag_last_reject_mask|=(uint8_t)(1U<<i);
        if(dominant==0U)sat_inc16(&m_status.diag_reject_rms);else if(dominant==1U)sat_inc16(&m_status.diag_reject_peak);
        else if(dominant==2U)sat_inc16(&m_status.diag_reject_p2p);else sat_inc16(&m_status.diag_reject_zc);
        candidate_init(m);return;
    }
    sat_inc16(&m_status.diag_candidate_matches);
    uint16_t n=(uint16_t)(m_candidate.count+1U);
    m_candidate.rms=incremental_mean(m_candidate.rms,m->rms_mg,n);
    m_candidate.peak=incremental_mean(m_candidate.peak,m->peak_mg,n);
    m_candidate.p2p=incremental_mean(m_candidate.p2p,m->peak_to_peak_mg,n);
    m_candidate.zc=incremental_mean(m_candidate.zc,m->zero_cross_hz,n);
    m_candidate.dev_rms=incremental_dev(m_candidate.dev_rms,uabs16diff(m->rms_mg,m_candidate.rms),n);
    m_candidate.dev_peak=incremental_dev(m_candidate.dev_peak,uabs16diff(m->peak_mg,m_candidate.peak),n);
    m_candidate.dev_p2p=incremental_dev(m_candidate.dev_p2p,uabs16diff(m->peak_to_peak_mg,m_candidate.p2p),n);
    m_candidate.dev_zc=incremental_dev(m_candidate.dev_zc,uabs16diff(m->zero_cross_hz,m_candidate.zc),n);
    if(m_candidate.count<255U)m_candidate.count++;
    m_status.candidate_count=m_candidate.count;
}

static int best_profile(nrfclaw_accel_vibration_metrics_t const *m,uint16_t *score_out){
    int best=-1;uint16_t bs=65535U;
    for(uint8_t i=0;i<m_profile_count;i++){
        uint16_t s=profile_score(&m_profiles[i],m);
        if(s<bs){bs=s;best=(int)i;}
    }
    if(score_out)*score_out=bs;
    return best;
}

static int best_profile_discovery(nrfclaw_accel_vibration_metrics_t const *m,uint16_t *score_out){
    int best=-1;uint16_t bs=65535U;
    for(uint8_t i=0;i<m_profile_count;i++){
        uint16_t s=discovery_profile_score(&m_profiles[i],m);
        if(s<bs){bs=s;best=(int)i;}
    }
    if(score_out)*score_out=bs;
    return best;
}

static void update_noise(nrfclaw_accel_vibration_metrics_t const *m){
    if(m_quiet_count==0U){m_quiet_rms=m->rms_mg;m_quiet_peak=m->peak_mg;m_quiet_count=1U;return;}
    uint16_t n=m_quiet_count<255U?(uint16_t)(m_quiet_count+1U):255U;
    m_quiet_rms=incremental_mean(m_quiet_rms,m->rms_mg,n);m_quiet_peak=incremental_mean(m_quiet_peak,m->peak_mg,n);
    if(m_quiet_count<255U)m_quiet_count++;
}
static uint16_t dynamic_off_rms(void){
    uint32_t floor=m_cfg.off_rms_mg;
    if(m_quiet_count>=3U){uint32_t q=(uint32_t)m_quiet_rms*3U+3U;if(q>floor)floor=q;}
    return clamp16u32(floor);
}
static bool machine_off(nrfclaw_accel_vibration_metrics_t const *m){
    if(!m)return true;
    uint16_t rf=dynamic_off_rms();uint32_t pl=(uint32_t)rf*5U;
    if(m_quiet_count>=3U){uint32_t qp=(uint32_t)m_quiet_peak*3U+10U;if(qp>pl)pl=qp;}
    if(pl<20U)pl=20U;
    bool off=m->rms_mg<=rf&&m->peak_mg<=pl;
    if(off&&m->rms_mg<=(uint16_t)(m_cfg.off_rms_mg*2U+2U))update_noise(m);
    return off;
}

static bool arm_machine_wake(nrfclaw_vib_auto_arm_reason_t reason);
static bool start_arming_or_wait(void);

static void timer_handler(void *ctx){(void)ctx;sat_inc16(&m_status.diag_timer_fires);m_timer_due=true;}
static void schedule_seconds(uint32_t seconds,sample_purpose_t purpose){
    trace_add(TRACE_TIMER_SCHEDULE,0U);
    sat_inc16(&m_status.diag_schedule_calls);m_status.diag_last_schedule_s=(uint16_t)(seconds>65535U?65535U:seconds);m_status.diag_last_schedule_purpose=(uint8_t)purpose;
    sat_inc16(&m_status.diag_timer_stop_calls);(void)app_timer_stop(m_va_timer);m_timer_due=false;m_delay_remaining_s=seconds;m_sample_purpose=purpose;
    m_status.next_sample_s=(uint16_t)(seconds>65535U?65535U:seconds);
    uint32_t chunk=seconds>VA_TIMER_CHUNK_S?VA_TIMER_CHUNK_S:seconds;
    if(chunk==0U){m_timer_due=true;return;}
    m_delay_remaining_s-=chunk;uint32_t rc=app_timer_start(m_va_timer,APP_TIMER_TICKS(chunk*1000UL),NULL);
    if(rc==NRF_SUCCESS)sat_inc16(&m_status.diag_timer_start_ok);else sat_inc16(&m_status.diag_timer_start_fail);
}
static void schedule_milliseconds(uint32_t milliseconds,sample_purpose_t purpose){
    trace_add(TRACE_TIMER_SCHEDULE,0U);
    sat_inc16(&m_status.diag_schedule_calls);m_status.diag_last_schedule_s=(uint16_t)((milliseconds+999U)/1000U);m_status.diag_last_schedule_purpose=(uint8_t)purpose;
    sat_inc16(&m_status.diag_timer_stop_calls);(void)app_timer_stop(m_va_timer);m_timer_due=false;m_delay_remaining_s=0U;m_sample_purpose=purpose;
    m_status.next_sample_s=(uint16_t)((milliseconds+999U)/1000U);
    if(milliseconds==0U){m_timer_due=true;return;}
    uint32_t rc=app_timer_start(m_va_timer,APP_TIMER_TICKS(milliseconds),NULL);
    if(rc==NRF_SUCCESS)sat_inc16(&m_status.diag_timer_start_ok);else sat_inc16(&m_status.diag_timer_start_fail);
}

static void service_timer_due(void){
    if(!m_timer_due)return;
    trace_add(TRACE_TIMER_DUE,0U);
    m_timer_due=false;
    if(m_delay_remaining_s){uint32_t chunk=m_delay_remaining_s>VA_TIMER_CHUNK_S?VA_TIMER_CHUNK_S:m_delay_remaining_s;m_delay_remaining_s-=chunk;
        uint32_t rem=m_delay_remaining_s+chunk;m_status.next_sample_s=(uint16_t)(rem>65535U?65535U:rem);
        uint32_t rc=app_timer_start(m_va_timer,APP_TIMER_TICKS(chunk*1000UL),NULL);if(rc==NRF_SUCCESS)sat_inc16(&m_status.diag_timer_start_ok);else sat_inc16(&m_status.diag_timer_start_fail);return;}
    m_status.next_sample_s=0U;
    if(m_sample_purpose==PURPOSE_ARMING){
        m_sample_purpose=PURPOSE_NONE;
        sat_inc16(&m_status.diag_arming_completes);
        if(!m_status.discovery_complete&&!m_discovery_started){m_discovery_started=true;m_discovery_started_at=nrfclaw_rtc_now();}
        trace_add(TRACE_ARMING_DONE,NRFCLAW_VIB_ARM_ARMING_DONE);
        (void)arm_machine_wake(NRFCLAW_VIB_ARM_ARMING_DONE);
        return;
    }
    if(m_sample_purpose!=PURPOSE_NONE){
        /* r3.8.8: active-state timers may expire while LIS2DH12 is kept in
         * low-power MOTION mode between FIFO windows. Release MOTION before
         * setting m_sampling so sampling_process() can enter FIFO mode instead
         * of remaining stuck forever in TIMER_DUE/MOTION. WAIT_MACHINE has no
         * sample timer, so this cannot steal the INT-first idle wake source. */
        if(nrfclaw_lis2dh12_mode()==NRFCLAW_ACCEL_MODE_MOTION){
            nrfclaw_lis2dh12_disable();
            sat_inc16(&m_status.diag_motion_releases);
            m_status.diag_probe_stage=2U;
        }
        m_status.diag_last_trigger=1U;m_sampling=true;
        sat_inc16(&m_status.diag_probe_requests);m_status.diag_probe_stage=1U;m_status.diag_probe_started_at=nrfclaw_rtc_now();
        if(m_sample_purpose==PURPOSE_DISCOVERY)sat_inc16(&m_status.diag_discovery_due);
    }
}

static uint32_t discovery_remaining(void){
    if(m_status.discovery_complete)return 0U;
    if(!m_discovery_started)return m_cfg.learning_time_s;
    uint32_t now=nrfclaw_rtc_now();uint32_t elapsed=now-m_discovery_started_at;
    return elapsed>=m_cfg.learning_time_s?0U:m_cfg.learning_time_s-elapsed;
}

static void fill_persist(nrfclaw_vib_auto_persist_t *p){
    memset(p,0,sizeof(*p));p->enabled=m_status.enabled?1U:0U;p->discovery_complete=m_status.discovery_complete?1U:0U;
    p->profile_count=m_profile_count;p->quiet_rms_mg=m_quiet_rms;p->quiet_peak_mg=m_quiet_peak;p->config=m_cfg;
    for(uint8_t i=0;i<m_profile_count;i++)p->profiles[i]=m_profiles[i].pub;
}
static void persist_now(void){nrfclaw_vib_auto_persist_t p;fill_persist(&p);(void)nrfclaw_vib_auto_store_request_save(&p);m_last_persist_s=nrfclaw_rtc_now();m_adapt_updates_since_save=0U;}
static void maybe_persist_aging(void){
    if(m_adapt_updates_since_save<VA_PERSIST_ADAPT_UPDATES)return;
    uint32_t now=nrfclaw_rtc_now();if((uint32_t)(now-m_last_persist_s)<VA_PERSIST_MIN_INTERVAL_S)return;persist_now();
}

static void restore_runtime_profile(runtime_profile_t *dst,nrfclaw_vib_auto_profile_t const *src){
    memset(dst,0,sizeof(*dst));dst->pub=*src;
    dst->dev_rms=max16((uint16_t)(src->rms_tol_mg/VA_MONITOR_DEV_MULT),1U);dst->dev_peak=max16((uint16_t)(src->peak_tol_mg/VA_MONITOR_DEV_MULT),1U);
    dst->dev_p2p=max16((uint16_t)(src->p2p_tol_mg/VA_MONITOR_DEV_MULT),1U);dst->dev_zc=max16((uint16_t)(src->zero_cross_tol_hz/VA_MONITOR_DEV_MULT),1U);
}
static void restore_persist(void){
    nrfclaw_vib_auto_persist_t p;if(!nrfclaw_vib_auto_store_load(&p))return;
    if(config_valid(&p.config))m_cfg=p.config;
    m_status.enabled=p.enabled!=0U;m_status.discovery_complete=p.discovery_complete!=0U;
    m_profile_count=p.profile_count;if(m_profile_count>m_cfg.max_profiles)m_profile_count=m_cfg.max_profiles;
    if(m_profile_count>NRFCLAW_VIB_AUTO_MAX_PROFILES)m_profile_count=NRFCLAW_VIB_AUTO_MAX_PROFILES;
    for(uint8_t i=0;i<m_profile_count;i++)restore_runtime_profile(&m_profiles[i],&p.profiles[i]);
    m_quiet_rms=p.quiet_rms_mg;m_quiet_peak=p.quiet_peak_mg;m_quiet_count=(m_quiet_rms||m_quiet_peak)?3U:0U;
}

static bool arm_machine_wake(nrfclaw_vib_auto_arm_reason_t reason){
    m_last_arm_reason=(uint8_t)reason;
    trace_add(TRACE_ARM_WAKE,(uint8_t)reason);
    sat_inc16(&m_status.diag_arm_wake_calls);
    if(!m_status.enabled){sat_inc16(&m_status.diag_arm_wake_fail);return false;}
    sat_inc16(&m_status.diag_timer_stop_calls);(void)app_timer_stop(m_va_timer);m_sampling=false;m_sample_purpose=PURPOSE_NONE;m_status.next_sample_s=0U;m_need_active_confirm=false;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
    if(!nrfclaw_lis2dh12_configure_motion_hp(m_cfg.wake_threshold_mg,m_cfg.wake_duration_ms,10U)){sat_inc16(&m_status.diag_arm_wake_fail);m_status.state=NRFCLAW_VIB_AUTO_ERROR;return false;}
    sat_inc16(&m_status.diag_arm_wake_ok);
    m_status.state=NRFCLAW_VIB_AUTO_WAIT_MACHINE;
    /* r3.8.5 INT-first: WAIT_MACHINE has no periodic FIFO probe.  The
     * LIS2DH12 HP motion interrupt is the only wake source. Timed sampling
     * begins only after a vibration window confirms that the machine is active. */
    return true;
}
static bool begin_window(void){
    if(nrfclaw_lis2dh12_mode()!=NRFCLAW_ACCEL_MODE_OFF)return false;
    nrfclaw_accel_config_t c={.mode=NRFCLAW_ACCEL_MODE_VIBRATION,.odr_hz=VA_SAMPLE_ODR_HZ,.full_scale_g=2U,.threshold_mg=0U,.duration_ms=0U,.low_power=false};
    return nrfclaw_lis2dh12_configure(&c);
}

static bool promote_candidate_if_possible(bool at_deadline){
    if(!m_candidate.valid||m_profile_count>=m_cfg.max_profiles)return false;
    uint8_t needed=m_cfg.candidate_confirmations;
    /* r2c: at the configured discovery deadline, two mutually-compatible
     * active windows are enough to seed the first profile.  This matters for
     * intermittent machines (rinse/pump cycles) where most sparse probes can
     * legitimately be quiet.  Before the deadline we retain the configured
     * confirmation count. */
    if(at_deadline&&needed>2U)needed=2U;
    if(m_candidate.count<needed)return false;
    runtime_profile_t *p=&m_profiles[m_profile_count];
    profile_init(p,m_candidate.rms,m_candidate.peak,m_candidate.p2p,m_candidate.zc,m_candidate.count,
                 m_candidate.dev_rms,m_candidate.dev_peak,m_candidate.dev_p2p,m_candidate.dev_zc);
    m_status.last_best_profile=m_profile_count;
    m_profile_count++;
    candidate_clear();
    return true;
}

static void discovery_finish(void){
    /* r2c/r3.8.7: never throw away the last valid candidate merely because
     * the calendar learning window expired.  First promote it when there is
     * enough evidence. */
    (void)promote_candidate_if_possible(true);

    if(m_profile_count==0U){
        /* r3.8.7 bug fix: latch the expired-deadline state before arming.
         * In r3.8.6 the main loop called discovery_finish() on every pass
         * because discovery_remaining() stayed at zero.  That repeatedly
         * called arm_machine_wake(), causing continuous I2C traffic and the
         * observed ~45 uA idle current.
         *
         * With this latch, the deadline remains expired (status still 0 s)
         * but the stopped machine is purely INT1-driven.  A real motion
         * event may still resume discovery and eventually promote a profile. */
        if(!m_discovery_deadline_wait){
            m_discovery_deadline_wait=true;
            m_status.state=NRFCLAW_VIB_AUTO_WAIT_MACHINE;
            (void)arm_machine_wake(NRFCLAW_VIB_ARM_DEADLINE_WAIT);
        }
        return;
    }

    m_discovery_deadline_wait=false;
    candidate_clear();
    m_status.discovery_complete=true;m_discovery_started=false;m_status.state=NRFCLAW_VIB_AUTO_MONITORING;persist_now();
    uint32_t obs=0;for(uint8_t i=0;i<m_profile_count;i++)obs+=m_profiles[i].pub.observations;
    emit(NRFCLAW_EVT_VIB_AUTO_LEARN_COMPLETE,m_profile_count,obs);
    schedule_seconds(m_cfg.normal_interval_s,PURPOSE_MONITOR);
}

static void maybe_finish_discovery(void){
    if(m_sampling||m_discovery_deadline_wait)return;
    if(!m_status.discovery_complete&&m_discovery_started&&discovery_remaining()==0U)discovery_finish();
}

static void discovery_accept(nrfclaw_accel_vibration_metrics_t const *m){
    uint16_t bests=65535U;int bi=best_profile_discovery(m,&bests);
    if(bi>=0&&bests<=m_cfg.profile_match_q8_8){sat_inc16(&m_status.diag_profile_matches);profile_discovery_update(&m_profiles[bi],m);m_status.last_best_profile=(uint8_t)bi;candidate_clear();return;}
    candidate_update(m);
    (void)promote_candidate_if_possible(false);
}

static void handle_discovery_metrics(nrfclaw_accel_vibration_metrics_t const *m){
    m_status.diag_probe_stage=6U;
    m_status.last_metrics=*m;nrfclaw_lis2dh12_disable();sat_inc16(&m_status.diag_total_windows);
    if(machine_off(m)){trace_add(TRACE_METRICS_QUIET,0U);sat_inc16(&m_status.diag_quiet_windows);emit(NRFCLAW_EVT_VIB_AUTO_MACHINE_OFF,m->rms_mg,0U);maybe_finish_discovery();if(!m_status.discovery_complete)(void)arm_machine_wake(NRFCLAW_VIB_ARM_DISCOVERY_QUIET);return;}
    trace_add(TRACE_METRICS_ACTIVE,0U);sat_inc16(&m_status.diag_active_windows);discovery_accept(m);
    /* After the configured learning deadline, new INT1-driven active windows
     * are still allowed to finish the first profile.  Use deadline promotion
     * semantics without reopening the expired timer window. */
    if(m_discovery_deadline_wait){
        (void)promote_candidate_if_possible(true);
        if(m_profile_count>0U){discovery_finish();return;}
    }else{
        maybe_finish_discovery();
        if(m_status.discovery_complete)return;
    }
    m_status.state=NRFCLAW_VIB_AUTO_DISCOVERY;trace_add(TRACE_DISCOVERY_SCHEDULE,0U);schedule_seconds(m_cfg.discovery_interval_s,PURPOSE_DISCOVERY);
}

static void adapt_component(uint16_t *mean,int32_t *residual,uint16_t sample,uint8_t shift){
    int32_t d=sdelta(sample,*mean);*residual+=d;int32_t step=*residual>>shift;
    if(step!=0){int32_t v=(int32_t)(*mean)+step;if(v<0)v=0;if(v>65535)v=65535;*mean=(uint16_t)v;*residual-=step<<shift;}
}
static void profile_slow_adapt(runtime_profile_t *p,nrfclaw_accel_vibration_metrics_t const *m){
    adapt_component(&p->pub.rms_mean_mg,&p->residual_rms,m->rms_mg,m_cfg.adaptation_shift);
    adapt_component(&p->pub.peak_mean_mg,&p->residual_peak,m->peak_mg,m_cfg.adaptation_shift);
    adapt_component(&p->pub.p2p_mean_mg,&p->residual_p2p,m->peak_to_peak_mg,m_cfg.adaptation_shift);
    adapt_component(&p->pub.zero_cross_mean_hz,&p->residual_zc,m->zero_cross_hz,m_cfg.adaptation_shift);
    if(p->pub.observations<65535U)p->pub.observations++;
    update_confidence(p);
    if(m_adapt_updates_since_save<65535U)m_adapt_updates_since_save++;
}

static uint32_t jittered_interval(uint32_t base){
    if(base<8U)return base;
    m_prng=m_prng*1664525UL+1013904223UL;
    int32_t span=(int32_t)(base/8U);
    int32_t off=(int32_t)(m_prng%(uint32_t)(span*2+1))-span;int32_t out=(int32_t)base+off;return out<1?1U:(uint32_t)out;
}
static uint32_t adaptive_normal_interval(void){
    uint32_t mul=1U;if(m_cfg.stable_expand_after>0U&&m_status.stable_streak>=m_cfg.stable_expand_after)mul=2U;
    if(m_status.stable_streak>=(uint16_t)(m_cfg.stable_expand_after*4U))mul=4U;
    if(mul>m_cfg.max_interval_multiplier)mul=m_cfg.max_interval_multiplier;
    uint32_t base=(uint32_t)m_cfg.normal_interval_s*mul;if(base>65535UL)base=65535UL;return jittered_interval(base);
}

static void handle_monitor_metrics(nrfclaw_accel_vibration_metrics_t const *m){
    m_status.last_metrics=*m;nrfclaw_lis2dh12_disable();
    if(machine_off(m)){m_status.consecutive_bad=0U;m_status.stable_streak=0U;m_alarm_recovery_good=0U;emit(NRFCLAW_EVT_VIB_AUTO_MACHINE_OFF,m->rms_mg,0U);(void)arm_machine_wake(NRFCLAW_VIB_ARM_MONITOR_QUIET);return;}
    uint16_t score=65535U;int bi=best_profile(m,&score);m_status.last_score_q8_8=score;m_status.last_best_profile=bi>=0?(uint8_t)bi:NRFCLAW_VIB_AUTO_PROFILE_NONE;
    if(bi<0){m_status.state=NRFCLAW_VIB_AUTO_ERROR;return;}
    if(score<warning_threshold()){
        if(m_status.consecutive_bad)m_status.consecutive_bad=0U;
        if(m_status.stable_streak<65535U)m_status.stable_streak++;
        if(score<=m_cfg.adapt_limit_q8_8){profile_slow_adapt(&m_profiles[bi],m);maybe_persist_aging();}
        if(m_status.state==NRFCLAW_VIB_AUTO_ALARM){
            if(++m_alarm_recovery_good<VA_ALARM_RECOVERY_GOOD){schedule_seconds(m_cfg.suspicious_interval_s,PURPOSE_MONITOR);return;}
        }
        m_alarm_recovery_good=0U;m_status.state=NRFCLAW_VIB_AUTO_MONITORING;schedule_seconds(adaptive_normal_interval(),PURPOSE_MONITOR);return;
    }
    m_status.stable_streak=0U;m_alarm_recovery_good=0U;
    if(m_status.consecutive_bad<255U)m_status.consecutive_bad++;
    if(score>=alarm_threshold()&&m_status.consecutive_bad>=m_cfg.alarm_consecutive){
        if(m_status.state!=NRFCLAW_VIB_AUTO_ALARM)emit(NRFCLAW_EVT_VIB_AUTO_ALARM,score,(uint32_t)(uint8_t)bi);
        m_status.state=NRFCLAW_VIB_AUTO_ALARM;
    } else {
        if(m_status.state!=NRFCLAW_VIB_AUTO_SUSPICIOUS)emit(NRFCLAW_EVT_VIB_AUTO_WARNING,score,(uint32_t)(uint8_t)bi);
        m_status.state=NRFCLAW_VIB_AUTO_SUSPICIOUS;
    }
    schedule_seconds(m_cfg.suspicious_interval_s,PURPOSE_MONITOR);
}

/* r3.8.13: robust start confirmation.  The first INT1-triggered active
 * window is never fed into discovery/profile learning.  A second FIFO after
 * VA_ACTIVE_CONFIRM_DELAY_S confirms continuity.  If that confirmation window
 * happens to be quiet (phase/weak interval), allow exactly one low-cost retry
 * after VA_ACTIVE_CONFIRM_RETRY_MS before declaring the trigger transient.
 * The LIS remains OFF between all confirmation windows.
 */
static void dispatch_completed_metrics(nrfclaw_accel_vibration_metrics_t const *m){
    if(m_need_active_confirm){
        m_need_active_confirm=false;
        nrfclaw_lis2dh12_disable();
        if(machine_off(m)){
            sat_inc16(&m_status.diag_confirm_rejects);trace_add(TRACE_CONFIRM_REJECT,0U);
            m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
            (void)arm_machine_wake(NRFCLAW_VIB_ARM_CONFIRM_REJECT);
            return;
        }
        sat_inc16(&m_status.diag_confirm_starts);trace_add(TRACE_CONFIRM_SCHEDULE,0U);
        m_active_confirm_pending=true;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=true;
        m_status.state=NRFCLAW_VIB_AUTO_ACTIVE_CONFIRM;
        schedule_seconds(VA_ACTIVE_CONFIRM_DELAY_S,m_sample_purpose);
        return;
    }
    if(m_active_confirm_pending){
        nrfclaw_lis2dh12_disable();
        if(machine_off(m)){
            if(m_active_confirm_retry_count<VA_ACTIVE_CONFIRM_MAX_RETRIES){
                m_active_confirm_retry_count++;
                sat_inc16(&m_status.diag_confirm_retries);trace_add(TRACE_CONFIRM_RETRY,0U);
                m_status.state=NRFCLAW_VIB_AUTO_ACTIVE_CONFIRM;m_status.diag_confirm_pending=true;
                schedule_milliseconds(VA_ACTIVE_CONFIRM_RETRY_MS,m_sample_purpose);
                return;
            }
            sat_inc16(&m_status.diag_confirm_rejects);trace_add(TRACE_CONFIRM_REJECT,0U);
            m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
            (void)arm_machine_wake(NRFCLAW_VIB_ARM_CONFIRM_REJECT);
            return;
        }
        if(m_active_confirm_retry_count>0U){sat_inc16(&m_status.diag_confirm_retry_ok);trace_add(TRACE_CONFIRM_RETRY_OK,0U);}
        sat_inc16(&m_status.diag_confirm_ok);trace_add(TRACE_CONFIRM_OK,0U);
        m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
        emit(NRFCLAW_EVT_VIB_AUTO_MACHINE_ON,m->rms_mg,0U);
        /* Continue below: only this confirmed active window can seed discovery
         * or enter normal monitoring. */
    }
    if(m_sample_purpose==PURPOSE_DISCOVERY)handle_discovery_metrics(m);
    else if(m_sample_purpose==PURPOSE_MONITOR)handle_monitor_metrics(m);
}

static void sampling_process(void){
    if(!m_sampling)return;
    /* r3.8.11: driver watchdog aborted a FIFO that never became readable.
     * Do not immediately reconfigure VIBRATION and recreate the high-current
     * state.  Latch ERROR with the LIS already powered down. */
    if(nrfclaw_lis2dh12_vibration_watchdog_failed()){
        nrfclaw_lis2dh12_vibration_watchdog_clear_failed();
        sat_inc16(&m_status.diag_fifo_watchdog_failures);trace_add(TRACE_FIFO_WATCHDOG_FAIL,0U);
        m_sampling=false;m_status.diag_probe_stage=7U;m_status.state=NRFCLAW_VIB_AUTO_ERROR;
        return;
    }
    /* A driver-level INT2/watchdog capture powers the LIS down immediately
     * while preserving metrics in RAM. Consume those metrics before treating
     * OFF as a request to start another window. */
    nrfclaw_accel_vibration_metrics_t ready;
    if(nrfclaw_lis2dh12_vibration_metrics(&ready)){
        m_sampling=false;sat_inc16(&m_status.diag_metrics_ready);m_status.diag_probe_stage=5U;
        dispatch_completed_metrics(&ready);
        return;
    }
    nrfclaw_accel_mode_t mode=nrfclaw_lis2dh12_mode();
    /* r3.8.8: WAIT_MACHINE remains strictly INT-first and has no sample timer.
     * During confirmed-active DISCOVERY/MONITORING intervals, service_timer_due()
     * explicitly releases low-power MOTION to OFF before requesting the FIFO. */
    if(mode!=NRFCLAW_ACCEL_MODE_OFF&&mode!=NRFCLAW_ACCEL_MODE_VIBRATION)return;
    if(mode==NRFCLAW_ACCEL_MODE_OFF){
        trace_add(TRACE_FIFO_BEGIN,0U);
        m_status.diag_probe_stage=3U;
        if(!begin_window()){sat_inc16(&m_status.diag_window_config_fail);m_status.diag_probe_stage=7U;m_sampling=false;m_status.state=NRFCLAW_VIB_AUTO_ERROR;}
        else {sat_inc16(&m_status.diag_window_config_ok);m_status.diag_probe_stage=4U;trace_add(TRACE_FIFO_CONFIG_OK,0U);}
        return;
    }
    if(!nrfclaw_lis2dh12_vibration_service())return;
    nrfclaw_accel_vibration_metrics_t m;if(!nrfclaw_lis2dh12_vibration_metrics(&m))return;m_sampling=false;sat_inc16(&m_status.diag_metrics_ready);m_status.diag_probe_stage=5U;
    dispatch_completed_metrics(&m);
}

void nrfclaw_vib_auto_init(void){
    memset(&m_status,0,sizeof(m_status));memset(m_profiles,0,sizeof(m_profiles));candidate_clear();default_config();
    m_profile_count=0U;m_quiet_rms=m_quiet_peak=m_quiet_count=0U;m_discovery_started=false;m_discovery_deadline_wait=false;m_timer_due=false;m_delay_remaining_s=0U;
    m_sample_purpose=PURPOSE_NONE;m_sampling=false;m_alarm_recovery_good=0U;m_prng=0x6E524656UL;m_adapt_updates_since_save=0U;m_last_persist_s=0U;
    memset(m_trace,0,sizeof(m_trace));m_trace_head=0U;m_trace_count=0U;m_last_arm_reason=0U;m_need_active_confirm=false;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;
#if VIB_AUTO_COMPILED
    APP_ERROR_CHECK(app_timer_create(&m_va_timer,APP_TIMER_MODE_SINGLE_SHOT,timer_handler));nrfclaw_vib_auto_store_init();restore_persist();
    m_status.profile_count=m_profile_count;m_status.last_best_profile=NRFCLAW_VIB_AUTO_PROFILE_NONE;
    if(m_status.enabled){
        if(!m_status.discovery_complete)m_discovery_started=false;
        (void)start_arming_or_wait();
    }else m_status.state=NRFCLAW_VIB_AUTO_DISABLED;
#else
    m_status.state=NRFCLAW_VIB_AUTO_DISABLED;
#endif
}

static bool start_arming_or_wait(void){
    sat_inc16(&m_status.diag_timer_stop_calls);(void)app_timer_stop(m_va_timer);
    nrfclaw_lis2dh12_disable();m_sampling=false;m_sample_purpose=PURPOSE_NONE;m_status.next_sample_s=0U;
    if(m_cfg.arming_delay_s==0UL){
        if(!m_status.discovery_complete&&!m_discovery_started){m_discovery_started=true;m_discovery_started_at=nrfclaw_rtc_now();}
        return arm_machine_wake(NRFCLAW_VIB_ARM_START_NO_DELAY);
    }
    sat_inc16(&m_status.diag_arming_starts);m_status.state=NRFCLAW_VIB_AUTO_ARMING;
    schedule_seconds(m_cfg.arming_delay_s,PURPOSE_ARMING);
    return true;
}

uint8_t nrfclaw_vib_auto_build_gate(void){return VIB_AUTO_COMPILED?1U:0U;}
uint8_t nrfclaw_vib_auto_r2_build_gate(void){return VIB_AUTO_COMPILED?5U:0U;}
bool nrfclaw_vib_auto_supported(void){return VIB_AUTO_COMPILED&&nrfclaw_lis2dh12_present()&&nrfclaw_vib_health_supported();}

bool nrfclaw_vib_auto_start(bool relearn){
#if VIB_AUTO_COMPILED
    if(!nrfclaw_vib_auto_supported())return false;
    (void)app_timer_stop(m_va_timer);
    nrfclaw_lis2dh12_disable();m_sampling=false;m_sample_purpose=PURPOSE_NONE;m_need_active_confirm=false;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
    m_status.enabled=true;m_status.consecutive_bad=0U;m_status.stable_streak=0U;m_alarm_recovery_good=0U;
    if(relearn){memset(m_profiles,0,sizeof(m_profiles));m_profile_count=0U;candidate_clear();diagnostics_reset();m_status.discovery_complete=false;m_discovery_started=false;m_discovery_deadline_wait=false;m_quiet_rms=m_quiet_peak=m_quiet_count=0U;}
    /* r3.8.5: relearn enters an installation arming delay. Learning time starts
     * only after that delay, so transport/fixation motion cannot seed profiles. */
    if(!m_status.discovery_complete)m_discovery_started=false;
    persist_now();return start_arming_or_wait();
#else
    (void)relearn;return false;
#endif
}

void nrfclaw_vib_auto_stop(void){
#if VIB_AUTO_COMPILED
    (void)app_timer_stop(m_va_timer);nrfclaw_lis2dh12_disable();m_sampling=false;m_sample_purpose=PURPOSE_NONE;m_need_active_confirm=false;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;m_status.enabled=false;m_discovery_deadline_wait=false;m_status.state=NRFCLAW_VIB_AUTO_DISABLED;m_status.next_sample_s=0U;persist_now();
#endif
}

bool nrfclaw_vib_auto_reset_learning(void){
#if VIB_AUTO_COMPILED
    memset(m_profiles,0,sizeof(m_profiles));m_profile_count=0U;candidate_clear();diagnostics_reset();m_status.discovery_complete=false;m_discovery_started=false;m_discovery_deadline_wait=false;m_status.last_best_profile=NRFCLAW_VIB_AUTO_PROFILE_NONE;m_quiet_rms=m_quiet_peak=m_quiet_count=0U;
    persist_now();
    if(m_status.enabled)return start_arming_or_wait();
    return true;
#else
    return false;
#endif
}

void nrfclaw_vib_auto_get_config(nrfclaw_vib_auto_config_t *out){if(out)*out=m_cfg;}
bool nrfclaw_vib_auto_set_config(nrfclaw_vib_auto_config_t const *cfg){
#if VIB_AUTO_COMPILED
    if(!config_valid(cfg))return false;
    m_cfg=*cfg;
    m_cfg.confirm_delay_s=(uint8_t)(m_cfg.arming_delay_s>255UL?255U:m_cfg.arming_delay_s);
    if(m_profile_count>m_cfg.max_profiles)m_profile_count=m_cfg.max_profiles;
    persist_now();
    if(m_status.enabled&&m_status.state==NRFCLAW_VIB_AUTO_WAIT_MACHINE)return arm_machine_wake(NRFCLAW_VIB_ARM_CONFIG_REAPPLY);
    if(m_status.enabled&&m_status.state==NRFCLAW_VIB_AUTO_ARMING)return start_arming_or_wait();
    return true;
#else
    (void)cfg;return false;
#endif
}

void nrfclaw_vib_auto_get_status(nrfclaw_vib_auto_status_t *out){
    if(!out)return;
    *out=m_status;
    out->profile_count=m_profile_count;out->candidate_count=m_candidate.count;
    out->discovery_remaining_s=discovery_remaining();out->quiet_rms_mg=m_quiet_rms;
    out->persistence_busy=nrfclaw_vib_auto_store_busy();
    out->diag_probe_active=m_sampling;out->diag_sample_purpose=(uint8_t)m_sample_purpose;out->diag_accel_mode=(uint8_t)nrfclaw_lis2dh12_mode();
    {uint32_t age=(m_sampling&&m_status.diag_probe_started_at)?(nrfclaw_rtc_now()-m_status.diag_probe_started_at):0U;out->diag_probe_age_s=(uint16_t)(age>65535U?65535U:age);}
    out->diag_arming_active=(m_status.state==NRFCLAW_VIB_AUTO_ARMING);
    out->diag_confirm_pending=m_active_confirm_pending;out->diag_confirm_delay_s=VA_ACTIVE_CONFIRM_DELAY_S;out->diag_confirm_retry_delay_ms=VA_ACTIVE_CONFIRM_RETRY_MS;
    out->diag_arming_remaining_s=out->diag_arming_active?m_status.next_sample_s:0U;
    out->diag_candidate_valid=m_candidate.valid;
    if(m_candidate.valid){
        out->diag_candidate_mean[0]=m_candidate.rms;out->diag_candidate_mean[1]=m_candidate.peak;out->diag_candidate_mean[2]=m_candidate.p2p;out->diag_candidate_mean[3]=m_candidate.zc;
        out->diag_candidate_tol[0]=max16(discovery_tol_from_dev(m_candidate.dev_rms,VA_MIN_RMS_TOL_MG),(uint16_t)(VA_MIN_RMS_TOL_MG*2U));
        out->diag_candidate_tol[1]=max16(discovery_tol_from_dev(m_candidate.dev_peak,VA_MIN_PEAK_TOL_MG),(uint16_t)(VA_MIN_PEAK_TOL_MG*2U));
        out->diag_candidate_tol[2]=max16(discovery_tol_from_dev(m_candidate.dev_p2p,VA_MIN_P2P_TOL_MG),(uint16_t)(VA_MIN_P2P_TOL_MG*2U));
        out->diag_candidate_tol[3]=max16(discovery_tol_from_dev(m_candidate.dev_zc,VA_MIN_ZC_TOL_HZ),(uint16_t)(VA_MIN_ZC_TOL_HZ*2U));
    }else{memset(out->diag_candidate_mean,0,sizeof(out->diag_candidate_mean));memset(out->diag_candidate_tol,0,sizeof(out->diag_candidate_tol));}
}
bool nrfclaw_vib_auto_get_profile(uint8_t index,nrfclaw_vib_auto_profile_t *out){if(!out||index>=m_profile_count)return false;*out=m_profiles[index].pub;return true;}
uint8_t nrfclaw_vib_auto_profile_count(void){return m_profile_count;}
uint8_t nrfclaw_vib_auto_trace_count(void){return m_trace_count;}
bool nrfclaw_vib_auto_trace_get(uint8_t chronological_index,nrfclaw_vib_auto_trace_entry_t *out){
    if(!out||chronological_index>=m_trace_count)return false;
    uint8_t oldest=(uint8_t)((m_trace_head+NRFCLAW_VIB_AUTO_TRACE_LEN-m_trace_count)%NRFCLAW_VIB_AUTO_TRACE_LEN);
    *out=m_trace[(uint8_t)((oldest+chronological_index)%NRFCLAW_VIB_AUTO_TRACE_LEN)];return true;
}
uint8_t nrfclaw_vib_auto_last_arm_reason(void){return m_last_arm_reason;}

bool nrfclaw_vib_auto_get_baseline(nrfclaw_vib_model_t *out){
    if(!out||m_profile_count==0U)return false;
    nrfclaw_vib_auto_profile_t const *p=&m_profiles[0].pub;
    out->rms_mean_mg=p->rms_mean_mg;out->rms_tol_mg=p->rms_tol_mg;out->peak_mean_mg=p->peak_mean_mg;out->peak_tol_mg=p->peak_tol_mg;
    out->p2p_mean_mg=p->p2p_mean_mg;out->p2p_tol_mg=p->p2p_tol_mg;out->zero_cross_mean_hz=p->zero_cross_mean_hz;out->zero_cross_tol_hz=p->zero_cross_tol_hz;return true;
}

void nrfclaw_vib_auto_on_event(nrfclaw_event_t const *e){
#if VIB_AUTO_COMPILED
    if(!e||!m_status.enabled)return;
    if(e->type==NRFCLAW_EVT_ACCEL_MOTION){
        sat_inc16(&m_status.diag_motion_events);
        if(m_status.state==NRFCLAW_VIB_AUTO_ARMING){sat_inc16(&m_status.diag_motion_ignored_arming);trace_add(TRACE_MOTION_IGNORED,0U);return;}
        if(m_status.state!=NRFCLAW_VIB_AUTO_WAIT_MACHINE){sat_inc16(&m_status.diag_motion_ignored_state);trace_add(TRACE_MOTION_IGNORED,0U);return;}
        sat_inc16(&m_status.diag_motion_accepted);sat_inc16(&m_status.diag_int_first_windows);m_status.diag_last_trigger=2U;trace_add(TRACE_MOTION_ACCEPT,0U);
        nrfclaw_lis2dh12_disable();trace_add(TRACE_INT_DISABLE,0U);
        m_need_active_confirm=true;m_active_confirm_pending=false;m_active_confirm_retry_count=0U;m_status.diag_confirm_pending=false;
        sat_inc16(&m_status.diag_probe_requests);m_status.diag_probe_stage=1U;m_status.diag_probe_started_at=nrfclaw_rtc_now();
        if(!m_status.discovery_complete){
            if(!m_discovery_started){m_discovery_started=true;m_discovery_started_at=nrfclaw_rtc_now();}
            m_status.state=NRFCLAW_VIB_AUTO_DISCOVERY;m_sample_purpose=PURPOSE_DISCOVERY;m_sampling=true;
        }else{
            m_status.state=NRFCLAW_VIB_AUTO_MONITORING;m_sample_purpose=PURPOSE_MONITOR;m_sampling=true;
        }
    }
#else
    (void)e;
#endif
}

void nrfclaw_vib_auto_process(void){
#if VIB_AUTO_COMPILED
    nrfclaw_vib_auto_store_process();if(!m_status.enabled)return;maybe_finish_discovery();service_timer_due();sampling_process();
#endif
}
