#ifndef NRFCLAW_VM_SEMANTIC_STATE_H
#define NRFCLAW_VM_SEMANTIC_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrfclaw_capability.h"

typedef struct {
    uint16_t capability_id;
    uint8_t channel;
    uint8_t kind;
    uint8_t value_type;
    int8_t scale10;
    uint8_t unit;
    uint32_t raw_value;
    uint16_t generation;
} nrfclaw_vm_semantic_state_t;

/* B7.6f2k4: one retained RAM semantic-state slot owned by the active VM.
 * Persistence remains explicit in bytecode (PERSIST_LOAD/SAVE); BOOT programs
 * republish the restored value after reset. */
void nrfclaw_vm_semantic_state_clear(void);
bool nrfclaw_vm_semantic_state_publish(uint16_t capability_id,
                                        uint8_t channel,
                                        uint32_t raw_value);
bool nrfclaw_vm_semantic_state_snapshot(nrfclaw_vm_semantic_state_t *out);
bool nrfclaw_vm_semantic_state_get(uint16_t capability_id,
                                    uint8_t channel,
                                    nrfclaw_vm_semantic_state_t *out);

#endif
