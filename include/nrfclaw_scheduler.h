#ifndef NRFCLAW_SCHEDULER_H
#define NRFCLAW_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_schedule.h"

typedef enum
{
    NRFCLAW_SCHED_DISABLED = 0,
    NRFCLAW_SCHED_WAIT_TIME,
    NRFCLAW_SCHED_ARMED,
    NRFCLAW_SCHED_RUNNING,
    NRFCLAW_SCHED_EXPIRED,
    NRFCLAW_SCHED_ERROR,
    NRFCLAW_SCHED_SUSPENDED
} nrfclaw_scheduler_state_t;

void nrfclaw_scheduler_init(void);
bool nrfclaw_scheduler_set_rule(nrfclaw_schedule_t const *rule);
bool nrfclaw_scheduler_set_rule_deferred(nrfclaw_schedule_t const *rule);
void nrfclaw_scheduler_on_time_sync(void);
bool nrfclaw_scheduler_on_rtc_event(void);
void nrfclaw_scheduler_pause_for_manual_run(void);

/*
 * Sticky maintenance lock used by P0.21/NUS programming.
 * Autonomous schedules stay suspended until reboot.
 */
void nrfclaw_scheduler_suspend_for_programming(void);
bool nrfclaw_scheduler_is_programming_suspended(void);
void nrfclaw_scheduler_process(void);

nrfclaw_scheduler_state_t nrfclaw_scheduler_state(void);
nrfclaw_schedule_t nrfclaw_scheduler_rule(void);
uint32_t nrfclaw_scheduler_next_epoch(void);

#endif
