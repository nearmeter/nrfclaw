#ifndef NRFCLAW_VM_H
#define NRFCLAW_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrfclaw_event.h"

#define NRFCLAW_VM_MAX_PROGRAM_SIZE 1024U
#define NRFCLAW_VM_MAX_NOTIFY_STRING 64U

typedef enum {
    NRFCLAW_VM_STOPPED = 0,
    NRFCLAW_VM_READY,
    NRFCLAW_VM_WAIT_RTC,
    NRFCLAW_VM_WAIT_HALL,
    NRFCLAW_VM_WAIT_BATTERY,
    NRFCLAW_VM_WAIT_LORA,
    NRFCLAW_VM_DONE,
    NRFCLAW_VM_ERROR
} nrfclaw_vm_state_t;


typedef enum {
    NRFCLAW_VM_NOTIFY_U32 = 1,
    NRFCLAW_VM_NOTIFY_STRING = 2
} nrfclaw_vm_notify_type_t;

typedef bool (*nrfclaw_vm_notify_handler_t)(
    nrfclaw_vm_notify_type_t type,
    uint8_t channel,
    uint8_t const *data,
    uint16_t len);

typedef enum {
    NRFCLAW_UPLOAD_OK = 0,
    NRFCLAW_UPLOAD_BUSY,
    NRFCLAW_UPLOAD_SIZE,
    NRFCLAW_UPLOAD_OFFSET,
    NRFCLAW_UPLOAD_INCOMPLETE,
    NRFCLAW_UPLOAD_CRC,
    NRFCLAW_UPLOAD_INVALID_PROGRAM
} nrfclaw_upload_result_t;

void nrfclaw_vm_init(void);
void nrfclaw_vm_set_notify_handler(nrfclaw_vm_notify_handler_t handler);
void nrfclaw_vm_on_event(nrfclaw_event_t const *e);
void nrfclaw_vm_tick(void);

nrfclaw_vm_state_t nrfclaw_vm_state(void);
uint16_t nrfclaw_vm_pc(void);
uint16_t nrfclaw_vm_loaded_program_len(void);

bool nrfclaw_vm_stop(void);
bool nrfclaw_vm_run_loaded(void);

nrfclaw_upload_result_t nrfclaw_vm_upload_begin(uint16_t total_len,
                                                 uint16_t expected_crc);
nrfclaw_upload_result_t nrfclaw_vm_upload_write(uint16_t offset,
                                                 uint8_t const *data,
                                                 uint16_t len);
nrfclaw_upload_result_t nrfclaw_vm_upload_finish(void);

/* Kept for Stage-2 regression tests. */
bool nrfclaw_vm_start_validation_test(uint8_t test_id);

#endif
