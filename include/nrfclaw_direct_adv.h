#ifndef NRFCLAW_DIRECT_ADV_H
#define NRFCLAW_DIRECT_ADV_H

#include "nrfclaw_event.h"

/*
 * B7.6f Direct Advertising Telemetry/Events v2.
 *
 * v2 inserts an explicit HA transport role at byte 5.
 * Page 0 carries runtime telemetry; page 1 carries user-facing events;
 * page 3 carries one retained VM-published semantic state.
 */
typedef enum {
    NRFCLAW_DIRECT_EVENT_HALL = 1,
    NRFCLAW_DIRECT_EVENT_MOTION = 2,
    NRFCLAW_DIRECT_EVENT_TAP = 3,
    NRFCLAW_DIRECT_EVENT_FALL = 4,
    NRFCLAW_DIRECT_EVENT_WALK = 5,
    NRFCLAW_DIRECT_EVENT_VIBRATION_WARNING = 6,
    NRFCLAW_DIRECT_EVENT_VIBRATION_ALARM = 7,
    NRFCLAW_DIRECT_EVENT_MACHINE_ON = 8,
    NRFCLAW_DIRECT_EVENT_MACHINE_OFF = 9,
    NRFCLAW_DIRECT_EVENT_VIBRATION_LEARN_COMPLETE = 10
} nrfclaw_direct_event_type_t;

void nrfclaw_direct_adv_prepare_boot(uint8_t ha_role);
void nrfclaw_direct_adv_init(void);
void nrfclaw_direct_adv_on_event(nrfclaw_event_t const *evt);
void nrfclaw_direct_adv_process(void);

#endif
