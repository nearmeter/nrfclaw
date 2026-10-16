#ifndef NRFCLAW_RF_CAPABILITY_H
#define NRFCLAW_RF_CAPABILITY_H
#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_capability.h"
#include "nrfclaw_rfvm.h"

#define NRFCLAW_RF_CAP_MAX 16U
#define NRFCLAW_RF_SEMCAP_DEVICE_ID       0x0900U
#define NRFCLAW_RF_SEMCAP_CHANNEL         0x0901U
#define NRFCLAW_RF_SEMCAP_BATTERY_OK      0x0902U
#define NRFCLAW_RF_SEMCAP_WIND_RAW        0x0910U
#define NRFCLAW_RF_SEMCAP_GUST_RAW        0x0911U
#define NRFCLAW_RF_SEMCAP_WIND_DIRECTION  0x0912U
#define NRFCLAW_RF_SEMCAP_RAIN_RAW        0x0913U
#define NRFCLAW_RF_SEMCAP_UV_INDEX        0x0914U
#define NRFCLAW_RF_SEMCAP_BATTERY_MV      0x0920U
#define NRFCLAW_RF_SEMCAP_SOIL_MOISTURE   0x0921U
#define NRFCLAW_RF_SEMCAP_TX_BOOST        0x0922U
#define NRFCLAW_RF_SEMCAP_AD_RAW          0x0923U
#define NRFCLAW_RF_SEMCAP_CONDUCTIVITY_US_CM 0x0924U
#define NRFCLAW_RF_SEMCAP_WIND_SPEED_M_S       0x0930U
#define NRFCLAW_RF_SEMCAP_WIND_GUST_M_S        0x0931U
#define NRFCLAW_RF_SEMCAP_WIND_DIRECTION_DEG   0x0932U
#define NRFCLAW_RF_SEMCAP_RAIN_TOTAL_MM         0x0933U
#define NRFCLAW_RF_SEMCAP_RAIN_RATE_MM_H        0x0934U
#define NRFCLAW_RF_SEMCAP_LIGHTNING_COUNT       0x0936U
#define NRFCLAW_RF_SEMCAP_LIGHTNING_DISTANCE_KM 0x0937U
#define NRFCLAW_RF_SEMCAP_PM2_5_UG_M3           0x0938U
#define NRFCLAW_RF_SEMCAP_PM10_UG_M3            0x0939U

#define NRFCLAW_RF_SEMCAP_LAST_BUTTON_CODE      0x093AU
typedef struct {
    nrfclaw_capability_desc_t desc;
    nrfclaw_capability_value_t value;
} nrfclaw_rf_capability_entry_t;

void nrfclaw_rf_capability_clear(void);
bool nrfclaw_rf_capability_publish(uint8_t profile_id,
                                   nrfclaw_rfvm_result_t const *decoded);
uint8_t nrfclaw_rf_capability_count(void);
bool nrfclaw_rf_capability_at(uint8_t index,
                              nrfclaw_rf_capability_entry_t *out);
bool nrfclaw_rf_capability_get(uint16_t capability_id,
                               uint8_t channel,
                               nrfclaw_rf_capability_entry_t *out);
#endif
