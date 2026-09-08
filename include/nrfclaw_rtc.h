#ifndef NRFCLAW_RTC_H
#define NRFCLAW_RTC_H
#include <stdint.h>
#include <stdbool.h>

void nrfclaw_rtc_init(void);
void nrfclaw_rtc_set_epoch(uint32_t epoch_utc);
uint32_t nrfclaw_rtc_now(void);
bool nrfclaw_rtc_is_valid(void);
bool nrfclaw_rtc_set_alarm_epoch(uint32_t epoch_utc);
void nrfclaw_rtc_cancel_alarm(void);

#endif
