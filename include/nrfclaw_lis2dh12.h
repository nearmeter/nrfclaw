#ifndef NRFCLAW_LIS2DH12_H
#define NRFCLAW_LIS2DH12_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"

#define NRFCLAW_ACCEL_VIB_MAX_SAMPLES 32U

typedef struct { int16_t x_mg, y_mg, z_mg; } nrfclaw_lis2dh12_xyz_t;

typedef enum {
    NRFCLAW_ACCEL_MODE_OFF = 0,
    NRFCLAW_ACCEL_MODE_MOTION = 1,
    NRFCLAW_ACCEL_MODE_TAP = 2,
    NRFCLAW_ACCEL_MODE_FALL = 3,
    NRFCLAW_ACCEL_MODE_WALK = 4,       /* experimental SW-assisted classifier */
    NRFCLAW_ACCEL_MODE_VIBRATION = 5
} nrfclaw_accel_mode_t;

typedef struct {
    nrfclaw_accel_mode_t mode;
    uint16_t odr_hz;          /* 1,10,25,50,100,200,400 */
    uint8_t full_scale_g;     /* 2,4,8,16 */
    uint16_t threshold_mg;
    uint16_t duration_ms;
    bool low_power;
} nrfclaw_accel_config_t;

typedef struct {
    uint16_t sample_rate_hz;
    uint8_t sample_count;
    uint16_t rms_mg;
    uint16_t peak_mg;
    uint16_t peak_to_peak_mg;
    uint16_t zero_cross_hz; /* rough local indicator, NOT FFT dominant frequency */
    uint32_t sequence;
} nrfclaw_accel_vibration_metrics_t;

void nrfclaw_lis2dh12_init(void);
bool nrfclaw_lis2dh12_present(void);
uint8_t nrfclaw_lis2dh12_i2c_address(void);
uint8_t nrfclaw_lis2dh12_last_whoami(void);
bool nrfclaw_lis2dh12_read_xyz(nrfclaw_lis2dh12_xyz_t *xyz);

bool nrfclaw_lis2dh12_configure(const nrfclaw_accel_config_t *cfg);
/* EXPERIMENTAL VIB_AUTO wake primitive: HP-filtered INT1 motion. */
bool nrfclaw_lis2dh12_configure_motion_hp(uint16_t threshold_mg, uint16_t duration_ms, uint16_t odr_hz);
void nrfclaw_lis2dh12_disable(void);
nrfclaw_accel_mode_t nrfclaw_lis2dh12_mode(void);

/* Legacy Stage-9 API retained for bytecode compatibility. */
bool nrfclaw_lis2dh12_enable_motion(uint16_t threshold_mg, uint8_t duration_ticks);
void nrfclaw_lis2dh12_disable_motion(void);

bool nrfclaw_lis2dh12_vibration_metrics(nrfclaw_accel_vibration_metrics_t *out);
/* Experimental one-shot service: capture FIFO data even if INT2 was missed. */
bool nrfclaw_lis2dh12_vibration_service(void);
/* r3.8.11 autonomous FIFO watchdog state.  The watchdog is owned by the
 * LIS2DH12 driver so every VIBRATION consumer gets the same fail-safe. */
bool nrfclaw_lis2dh12_vibration_watchdog_failed(void);
void nrfclaw_lis2dh12_vibration_watchdog_clear_failed(void);
uint8_t nrfclaw_lis2dh12_vibration_count(void);
bool nrfclaw_lis2dh12_vibration_sample(uint8_t index, nrfclaw_lis2dh12_xyz_t *out);
void nrfclaw_lis2dh12_on_event(const nrfclaw_event_t *evt);
void nrfclaw_lis2dh12_on_int1_isr(void);
void nrfclaw_lis2dh12_on_int2_isr(void);
/* r3.8.3 low-overhead runtime diagnostics for autonomous vibration probes.
 * Counters are RAM-only and do not alter sensor behavior. */
typedef struct {
    uint16_t reg_read_failures;
    uint16_t reg_write_failures;
    uint16_t vibration_service_calls;
    uint16_t fifo_empty_polls;
    uint16_t capture_attempts;
    uint16_t capture_success;
    uint16_t sample_read_failures;
    uint16_t int1_isr_count;
    uint16_t int2_isr_count;
    uint16_t int2_event_captures;
    uint8_t  last_fifo_src;
    uint8_t  last_fifo_count;

    /* r3.8.6: snapshot taken after every successful HP-motion arm.
     * It is deliberately captured in the driver at the transition point so
     * reading diagnostics later cannot clear INT1_SRC or otherwise heal the
     * state being investigated. */
    uint16_t motion_snapshot_count;
    uint16_t motion_snapshot_fail;
    uint8_t motion_ctrl1;
    uint8_t motion_ctrl2;
    uint8_t motion_ctrl3;
    uint8_t motion_ctrl4;
    uint8_t motion_ctrl5;
    uint8_t motion_ctrl6;
    uint8_t motion_fifo_ctrl;
    uint8_t motion_int1_cfg;
    uint8_t motion_int1_ths;
    uint8_t motion_int1_duration;

    /* r3.8.9: global TWI low-power cleanup diagnostics. */
    uint16_t twi_low_power_releases;
    uint8_t  twi_last_psel_scl_disconnected;
    uint8_t  twi_last_psel_sda_disconnected;
    uint8_t  twi_last_errorsrc;
    uint8_t  twi_last_hfclk_running;
    uint32_t twi_last_scl_pin_cnf;
    uint32_t twi_last_sda_pin_cnf;

    /* r3.8.11: autonomous FIFO watchdog / missed-INT2 fallback. */
    uint16_t fifo_watchdog_arms;
    uint16_t fifo_watchdog_expirations;
    uint16_t fifo_watchdog_retries;
    uint16_t fifo_watchdog_aborts;
    uint16_t fifo_int2_completions;
    uint16_t fifo_timer_completions;
    uint16_t fifo_last_active_ms;
    uint8_t  fifo_last_completion_source; /* 0 none,1 INT2,2 timer,3 abort */
} nrfclaw_lis2dh12_diag_t;

typedef struct {
    uint8_t twi_enabled;
    uint8_t twi_psel_scl_disconnected;
    uint8_t twi_psel_sda_disconnected;
    uint8_t twi_errorsrc;
    uint8_t hfclk_running;
    uint32_t scl_pin_cnf;
    uint32_t sda_pin_cnf;
    uint8_t int1_level;
    uint8_t int2_level;
    uint8_t gpiote_events_port;
    uint8_t int1_latched;
    uint8_t int2_latched;
    uint8_t int1_sense;
    uint8_t int2_sense;
    uint8_t accel_mode;
    uint8_t scl_level;
    uint8_t sda_level;
} nrfclaw_lis2dh12_power_state_t;

void nrfclaw_lis2dh12_diag_reset(void);
void nrfclaw_lis2dh12_diag_get(nrfclaw_lis2dh12_diag_t *out);
/* r3.8.6 local-MCU power state only: no I2C transaction and no event clear. */
void nrfclaw_lis2dh12_power_state_get(nrfclaw_lis2dh12_power_state_t *out);

#endif
