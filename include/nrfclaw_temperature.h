#ifndef NRFCLAW_TEMPERATURE_H
#define NRFCLAW_TEMPERATURE_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"

typedef enum {
    NRFCLAW_TEMP_SOURCE_NONE = 0,
    NRFCLAW_TEMP_SOURCE_DS18B20 = 1,
    NRFCLAW_TEMP_SOURCE_NRF52_INTERNAL = 2
} nrfclaw_temperature_source_t;

void nrfclaw_temperature_init(void);
bool nrfclaw_temperature_available(void);
nrfclaw_temperature_source_t nrfclaw_temperature_source(void);
bool nrfclaw_temperature_busy(void);
bool nrfclaw_temperature_start(void);
bool nrfclaw_temperature_last_mC(int32_t *milli_c);
void nrfclaw_temperature_on_event(nrfclaw_event_t const *evt);
void nrfclaw_temperature_idle_lowpower(void);

#endif
