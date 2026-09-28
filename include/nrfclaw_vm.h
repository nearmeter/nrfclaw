#ifndef NRFCLAW_VM_H
#define NRFCLAW_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrfclaw_event.h"
#include "nrfclaw_schedule.h"
#include "nrfclaw_opcodes.h"


/*
 * ==========================================================================
 * VM configuration
 * ==========================================================================
 */

#define NRFCLAW_VM_MAX_PROGRAM_SIZE      1024U
#define NRFCLAW_VM_MAX_NOTIFY_STRING     64U
#define NRFCLAW_VM_BUFFER_SIZE            64U


/*
 * ==========================================================================
 * VM state
 * ==========================================================================
 */

typedef enum
{
    NRFCLAW_VM_STOPPED = 0,

    NRFCLAW_VM_READY,

    NRFCLAW_VM_WAIT_RTC,

    NRFCLAW_VM_WAIT_HALL,

    NRFCLAW_VM_WAIT_BATTERY,

    NRFCLAW_VM_WAIT_LORA,

    NRFCLAW_VM_WAIT_EVENT,

    NRFCLAW_VM_DONE,

    NRFCLAW_VM_ERROR,

    /*
     * Appended after ERROR intentionally so legacy numeric values for
     * STOPPED..ERROR remain unchanged in STATUS/NUS clients.
     */
    NRFCLAW_VM_WAIT_SERIAL_TX,
    NRFCLAW_VM_WAIT_DS18B20,
    NRFCLAW_VM_WAIT_LORA_RX,
    NRFCLAW_VM_WAIT_SERIAL_RX

} nrfclaw_vm_state_t;


/*
 * ==========================================================================
 * Program upload status
 * ==========================================================================
 */

typedef enum
{
    NRFCLAW_UPLOAD_OK = 0,

    NRFCLAW_UPLOAD_BUSY,

    NRFCLAW_UPLOAD_SIZE,

    NRFCLAW_UPLOAD_OFFSET,

    NRFCLAW_UPLOAD_INCOMPLETE,

    NRFCLAW_UPLOAD_CRC,

    NRFCLAW_UPLOAD_INVALID_PROGRAM,
    NRFCLAW_UPLOAD_AUTH_REQUIRED,
    NRFCLAW_UPLOAD_AUTH_FAILED

} nrfclaw_upload_result_t;


/*
 * ==========================================================================
 * VM notification types
 * ==========================================================================
 *
 * These are generic VM -> host notifications.
 *
 * They are intentionally independent from BLE.
 *
 * The VM calls a registered callback and the BLE/NUS layer decides
 * how to transport the result.
 */

typedef enum
{
    NRFCLAW_VM_NOTIFY_U32 = 1,

    NRFCLAW_VM_NOTIFY_STRING = 2

} nrfclaw_vm_notify_type_t;


/*
 * ==========================================================================
 * Notification callback
 * ==========================================================================
 *
 * type:
 *     U32 or STRING
 *
 * channel:
 *     logical application channel
 *
 * data:
 *     notification payload
 *
 * len:
 *     payload length
 *
 * Return:
 *
 *     true  -> accepted by transport
 *     false -> transport cannot accept notification
 */

typedef bool (*nrfclaw_vm_notify_handler_t)(
    nrfclaw_vm_notify_type_t type,
    uint8_t channel,
    uint8_t const *data,
    uint16_t len
);


/*
 * ==========================================================================
 * Core VM API
 * ==========================================================================
 */

void nrfclaw_vm_init(void);


void nrfclaw_vm_on_event(
    nrfclaw_event_t const *e
);


void nrfclaw_vm_tick(void);


nrfclaw_vm_state_t
nrfclaw_vm_state(void);


uint16_t
nrfclaw_vm_pc(void);


uint16_t
nrfclaw_vm_loaded_program_len(void);


/*
 * ==========================================================================
 * Runtime control
 * ==========================================================================
 */

bool
nrfclaw_vm_stop(void);


bool
nrfclaw_vm_run_loaded(void);


/* B7.6f2l3: reset one retained semantic accumulator without exposing VM
 * register/state-key implementation details to Home Assistant. */
bool nrfclaw_vm_semantic_accumulator_resettable(void);
bool nrfclaw_vm_semantic_accumulator_reset(void);


/*
 * Install an already validated/recovered program into the RAM execution slot.
 * The program is structurally validated again before it is accepted.
 */
bool nrfclaw_vm_install_program(
    uint8_t const *program,
    uint16_t len
);



/*
 * ==========================================================================
 * Program loader
 * ==========================================================================
 */

nrfclaw_upload_result_t
nrfclaw_vm_upload_begin(
    uint16_t total_len,
    uint16_t expected_crc
);


nrfclaw_upload_result_t
nrfclaw_vm_upload_write(
    uint16_t offset,
    uint8_t const *data,
    uint16_t len
);


nrfclaw_upload_result_t
nrfclaw_vm_upload_schedule(
    nrfclaw_schedule_t const *schedule
);


nrfclaw_upload_result_t
nrfclaw_vm_upload_auth_write(
    uint16_t offset,
    uint8_t const *data,
    uint16_t len
);


nrfclaw_upload_result_t
nrfclaw_vm_upload_finish(void);


/*
 * ==========================================================================
 * VM notification transport
 * ==========================================================================
 *
 * BLE initializes this with its own NUS notification handler.
 *
 * This keeps the VM independent from the transport implementation.
 */

void
nrfclaw_vm_set_notify_handler(
    nrfclaw_vm_notify_handler_t handler
);


/*
 * ==========================================================================
 * Stage-2 regression tests
 * ==========================================================================
 *
 * Kept only so the previous validation tests can still be executed.
 */

bool
nrfclaw_vm_start_validation_test(
    uint8_t test_id
);


#endif /* NRFCLAW_VM_H */
