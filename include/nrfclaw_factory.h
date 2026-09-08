#ifndef NRFCLAW_FACTORY_H
#define NRFCLAW_FACTORY_H
#include <stdbool.h>
void nrfclaw_factory_init(void);
bool nrfclaw_factory_request(void);
void nrfclaw_factory_process(void);
bool nrfclaw_factory_busy(void);
#endif
