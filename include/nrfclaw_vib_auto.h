#ifndef NRFCLAW_VIB_AUTO_H
#define NRFCLAW_VIB_AUTO_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_vib_health.h"

/* Experimental event namespace; does not alter the frozen core event enum. */
#define NRFCLAW_EVT_VIB_AUTO_MACHINE_ON      ((nrfclaw_event_type_t)0x40)
#define NRFCLAW_EVT_VIB_AUTO_LEARN_COMPLETE  ((nrfclaw_event_type_t)0x41)
#define NRFCLAW_EVT_VIB_AUTO_WARNING         ((nrfclaw_event_type_t)0x42)
#define NRFCLAW_EVT_VIB_AUTO_ALARM           ((nrfclaw_event_type_t)0x43)
#define NRFCLAW_EVT_VIB_AUTO_MACHINE_OFF     ((nrfclaw_event_type_t)0x44)

#define NRFCLAW_VIB_AUTO_MAX_PROFILES 6U
#define NRFCLAW_VIB_AUTO_PROFILE_NONE 0xFFU

typedef enum {
    NRFCLAW_VIB_AUTO_DISABLED = 0,
    NRFCLAW_VIB_AUTO_WAIT_MACHINE = 1,
    NRFCLAW_VIB_AUTO_DISCOVERY = 2,
    NRFCLAW_VIB_AUTO_MONITORING = 3,
    NRFCLAW_VIB_AUTO_SUSPICIOUS = 4,
    NRFCLAW_VIB_AUTO_ALARM = 5,
    NRFCLAW_VIB_AUTO_ERROR = 6,
    NRFCLAW_VIB_AUTO_ARMING = 7,
    NRFCLAW_VIB_AUTO_ACTIVE_CONFIRM = 8
} nrfclaw_vib_auto_state_t;

/*
 * r3.8.5 policy: after a configurable installation arming delay, WAIT_MACHINE
 * is strictly INT-first. LIS2DH12 HP-motion INT1 starts the first FIFO window;
 * periodic discovery/monitor timers exist only while a machine is confirmed
 * active. r3.8.8 adds arming_delay_s as the canonical installation delay so
 * delays may exceed 255 seconds. confirm_delay_s remains as a legacy 8-bit
 * compatibility mirror for older NDP clients/store migration; it no longer
 * delays the first FIFO window.
 */
typedef struct {
    uint32_t learning_time_s;          /* 5 min motor, 2 h washer, 24 h HVAC, ... */
    uint16_t discovery_interval_s;     /* sparse sampling during discovery */
    uint16_t normal_interval_s;        /* base monitoring interval */
    uint16_t suspicious_interval_s;    /* fast recheck after deviation */
    uint16_t wake_threshold_mg;        /* HP-filtered INT1 threshold */
    uint16_t wake_duration_ms;
    uint16_t off_rms_mg;               /* initial quiet floor; noise model refines it */
    uint16_t profile_match_q8_8;       /* discovery cluster match threshold */
    uint16_t adapt_limit_q8_8;         /* only very-normal samples may age a profile */
    uint8_t  confirm_delay_s;          /* legacy 0..255 s mirror; use arming_delay_s */
    uint8_t  candidate_confirmations;  /* repeated unknown windows before new profile */
    uint8_t  alarm_consecutive;        /* anomalous windows before alarm */
    uint8_t  max_profiles;             /* <= NRFCLAW_VIB_AUTO_MAX_PROFILES */
    uint8_t  sensitivity;              /* 0 low, 1 normal, 2 high */
    uint8_t  adaptation_shift;         /* EMA alpha ~= 1/(2^shift), default 11 */
    uint8_t  stable_expand_after;      /* stable samples before interval expansion */
    uint8_t  max_interval_multiplier;  /* 1,2,4... bounded low-power expansion */
    uint32_t arming_delay_s;           /* r3.8.8 canonical installation settle delay */
} nrfclaw_vib_auto_config_t;

typedef struct {
    uint16_t rms_mean_mg;
    uint16_t rms_tol_mg;
    uint16_t peak_mean_mg;
    uint16_t peak_tol_mg;
    uint16_t p2p_mean_mg;
    uint16_t p2p_tol_mg;
    uint16_t zero_cross_mean_hz;
    uint16_t zero_cross_tol_hz;
    uint16_t observations;
    uint8_t  confidence_pct;
    uint8_t  reserved;
} nrfclaw_vib_auto_profile_t;

typedef struct {
    bool enabled;
    bool discovery_complete;
    nrfclaw_vib_auto_state_t state;
    uint8_t profile_count;
    uint8_t candidate_count;
    uint8_t last_best_profile;
    uint8_t consecutive_bad;
    uint16_t last_score_q8_8;
    nrfclaw_accel_vibration_metrics_t last_metrics;
    uint16_t next_sample_s;
    uint32_t discovery_remaining_s;
    uint16_t stable_streak;
    uint16_t quiet_rms_mg;
    bool persistence_busy;

    /* r3.8.2 discovery instrumentation. Runtime-only: reset on relearn/reset,
     * intentionally not persisted so it cannot affect learning or flash wear. */
    uint16_t diag_total_windows;
    uint16_t diag_quiet_windows;
    uint16_t diag_active_windows;
    uint16_t diag_candidate_starts;
    uint16_t diag_candidate_matches;
    uint16_t diag_candidate_resets;
    uint16_t diag_profile_matches;
    uint16_t diag_reject_rms;
    uint16_t diag_reject_peak;
    uint16_t diag_reject_p2p;
    uint16_t diag_reject_zc;
    uint16_t diag_last_candidate_score_q8_8;
    uint16_t diag_last_feature_score_q8_8[4]; /* RMS, Peak, P2P, ZC */
    uint8_t  diag_last_reject_feature;        /* 0 none, 1 RMS, 2 Peak, 3 P2P, 4 ZC */
    uint8_t  diag_last_reject_mask;           /* bit0 RMS, bit1 Peak, bit2 P2P, bit3 ZC */
    bool     diag_candidate_valid;
    uint16_t diag_candidate_mean[4];          /* RMS, Peak, P2P, ZC */
    uint16_t diag_candidate_tol[4];           /* current discovery tolerances */

    /* r3.8.3 probe-pipeline instrumentation, RAM-only. */
    uint16_t diag_timer_fires;                /* raw app_timer expirations incl. chunks */
    uint16_t diag_discovery_due;              /* final discovery timer expirations */
    uint16_t diag_probe_requests;             /* discovery windows requested */
    uint16_t diag_motion_releases;            /* HP motion -> OFF transitions */
    uint16_t diag_window_config_ok;
    uint16_t diag_window_config_fail;
    uint16_t diag_metrics_ready;
    uint8_t  diag_probe_stage;                /* 0 idle,1 due,2 release,3 configure,4 FIFO,5 metrics,6 classified,7 error */
    uint32_t diag_probe_started_at;            /* RTC second when current probe was requested */
    bool     diag_probe_active;
    uint8_t  diag_sample_purpose;
    uint8_t  diag_accel_mode;
    uint16_t diag_probe_age_s;

    /* r3.8.4 scheduler/INT1 instrumentation, RAM-only. */
    uint16_t diag_schedule_calls;
    uint16_t diag_timer_stop_calls;
    uint16_t diag_timer_start_ok;
    uint16_t diag_timer_start_fail;
    uint16_t diag_motion_events;
    uint16_t diag_motion_accepted;
    uint16_t diag_motion_ignored_state;
    uint16_t diag_arm_wake_calls;
    uint16_t diag_arm_wake_ok;
    uint16_t diag_arm_wake_fail;
    uint16_t diag_last_schedule_s;
    uint8_t  diag_last_schedule_purpose;
    uint8_t  diag_last_trigger; /* 0 none, 1 timer, 2 INT1 motion */

    /* r3.8.5 INT-first / installation arming diagnostics, RAM-only. */
    uint16_t diag_arming_starts;
    uint16_t diag_arming_completes;
    uint16_t diag_motion_ignored_arming;
    uint16_t diag_int_first_windows;
    uint16_t diag_arming_remaining_s;
    bool     diag_arming_active;

    /* r3.8.11 VIB_AUTO reaction to driver-level FIFO watchdog. */
    uint16_t diag_fifo_watchdog_failures;

    /* r3.8.13 robust impact rejection / active confirmation, RAM-only. */
    uint16_t diag_confirm_starts;
    uint16_t diag_confirm_ok;
    uint16_t diag_confirm_rejects;
    uint16_t diag_confirm_retries;
    uint16_t diag_confirm_retry_ok;
    uint16_t diag_confirm_delay_s;
    uint16_t diag_confirm_retry_delay_ms;
    bool     diag_confirm_pending;
} nrfclaw_vib_auto_status_t;


/* r3.8.10: compact RAM-only state-transition trace. */
#define NRFCLAW_VIB_AUTO_TRACE_LEN 16U
typedef struct {
    uint32_t timestamp_s;
    uint8_t event;
    uint8_t state;
    uint8_t purpose;
    uint8_t accel_mode;
    uint8_t arm_reason;
    uint8_t reserved[3];
} nrfclaw_vib_auto_trace_entry_t;

typedef enum {
    NRFCLAW_VIB_ARM_NONE = 0,
    NRFCLAW_VIB_ARM_ARMING_DONE = 1,
    NRFCLAW_VIB_ARM_DISCOVERY_QUIET = 2,
    NRFCLAW_VIB_ARM_MONITOR_QUIET = 3,
    NRFCLAW_VIB_ARM_DEADLINE_WAIT = 4,
    NRFCLAW_VIB_ARM_START_NO_DELAY = 5,
    NRFCLAW_VIB_ARM_CONFIG_REAPPLY = 6,
    NRFCLAW_VIB_ARM_RESET_LEARNING = 7,
    NRFCLAW_VIB_ARM_CONFIRM_REJECT = 8
} nrfclaw_vib_auto_arm_reason_t;

void nrfclaw_vib_auto_init(void);
void nrfclaw_vib_auto_process(void);
void nrfclaw_vib_auto_on_event(nrfclaw_event_t const *evt);

bool nrfclaw_vib_auto_supported(void);
uint8_t nrfclaw_vib_auto_build_gate(void);
uint8_t nrfclaw_vib_auto_r2_build_gate(void);
bool nrfclaw_vib_auto_start(bool relearn);
void nrfclaw_vib_auto_stop(void);
bool nrfclaw_vib_auto_reset_learning(void);

void nrfclaw_vib_auto_get_config(nrfclaw_vib_auto_config_t *out);
bool nrfclaw_vib_auto_set_config(nrfclaw_vib_auto_config_t const *cfg);
void nrfclaw_vib_auto_get_status(nrfclaw_vib_auto_status_t *out);
bool nrfclaw_vib_auto_get_profile(uint8_t index, nrfclaw_vib_auto_profile_t *out);
uint8_t nrfclaw_vib_auto_profile_count(void);
uint8_t nrfclaw_vib_auto_trace_count(void);
bool nrfclaw_vib_auto_trace_get(uint8_t chronological_index, nrfclaw_vib_auto_trace_entry_t *out);
uint8_t nrfclaw_vib_auto_last_arm_reason(void);

/* r1 compatibility: exposes profile 0 as a vib-health model when available. */
bool nrfclaw_vib_auto_get_baseline(nrfclaw_vib_model_t *out);

#endif
