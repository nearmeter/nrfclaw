#ifndef NRFCLAW_VIB_HEALTH_H
#define NRFCLAW_VIB_HEALTH_H
#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_lis2dh12.h"

typedef struct {
    uint16_t rms_mean_mg, rms_tol_mg;
    uint16_t peak_mean_mg, peak_tol_mg;
    uint16_t p2p_mean_mg, p2p_tol_mg;
    uint16_t zero_cross_mean_hz, zero_cross_tol_hz;
} nrfclaw_vib_model_t;

typedef enum {
    NRFCLAW_VIB_HEALTH_UNKNOWN=0,
    NRFCLAW_VIB_HEALTH_NORMAL=1,
    NRFCLAW_VIB_HEALTH_WARNING=2,
    NRFCLAW_VIB_HEALTH_ALARM=3
} nrfclaw_vib_health_state_t;

typedef struct {
    bool model_valid;
    nrfclaw_vib_health_state_t state;
    uint16_t score_q8_8;
    nrfclaw_accel_vibration_metrics_t metrics;
} nrfclaw_vib_health_result_t;

void nrfclaw_vib_health_init(void);
uint8_t nrfclaw_vib_health_build_gate(void); /* r1c build-coherency diagnostic */
bool nrfclaw_vib_health_supported(void);
bool nrfclaw_vib_health_model_set(nrfclaw_vib_model_t const *model);
bool nrfclaw_vib_health_model_get(nrfclaw_vib_model_t *model);
void nrfclaw_vib_health_model_clear(void);
/* Start one short FIFO acquisition if no metrics are currently ready. */
bool nrfclaw_vib_health_prepare_sample(void);
bool nrfclaw_vib_health_score(nrfclaw_vib_health_result_t *result);
#endif
