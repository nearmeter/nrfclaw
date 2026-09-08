#ifndef NRFCLAW_DS18B20_H
#define NRFCLAW_DS18B20_H
#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_event.h"
void nrfclaw_ds18b20_init(void);
bool nrfclaw_ds18b20_present(void);
bool nrfclaw_ds18b20_probe(void);
bool nrfclaw_ds18b20_start(void);
bool nrfclaw_ds18b20_busy(void);
bool nrfclaw_ds18b20_last_mC(int32_t *milli_c);
void nrfclaw_ds18b20_on_event(const nrfclaw_event_t *evt);
void nrfclaw_ds18b20_idle_lowpower(void);
#endif
