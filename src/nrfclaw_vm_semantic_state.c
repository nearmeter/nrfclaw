#include "nrfclaw_vm_semantic_state.h"

#include <string.h>

static bool m_valid;
static nrfclaw_vm_semantic_state_t m_state;
static uint16_t m_generation;

void nrfclaw_vm_semantic_state_clear(void)
{
    m_valid = false;
    memset(&m_state, 0, sizeof(m_state));
}

bool nrfclaw_vm_semantic_state_publish(uint16_t capability_id,
                                        uint8_t channel,
                                        uint32_t raw_value)
{
    nrfclaw_capability_desc_t d;

    if (!nrfclaw_capability_descriptor(capability_id, channel, &d))
        return false;
    if ((d.behavior_flags & (NRFCLAW_CAP_BEHAVIOR_REPORTABLE |
                             NRFCLAW_CAP_BEHAVIOR_RETAINED)) !=
                            (NRFCLAW_CAP_BEHAVIOR_REPORTABLE |
                             NRFCLAW_CAP_BEHAVIOR_RETAINED))
        return false;
    if (nrfclaw_capability_value_size(d.value_type) == 0U ||
        nrfclaw_capability_value_size(d.value_type) > 4U)
        return false;

    m_generation++;
    if (m_generation == 0U)
        m_generation++;

    m_state.capability_id = capability_id;
    m_state.channel = channel;
    m_state.kind = d.kind;
    m_state.value_type = d.value_type;
    m_state.scale10 = d.scale10;
    m_state.unit = d.unit;
    m_state.raw_value = raw_value;
    m_state.generation = m_generation;
    m_valid = true;
    return true;
}

bool nrfclaw_vm_semantic_state_snapshot(nrfclaw_vm_semantic_state_t *out)
{
    if (!out || !m_valid)
        return false;
    *out = m_state;
    return true;
}

bool nrfclaw_vm_semantic_state_get(uint16_t capability_id,
                                    uint8_t channel,
                                    nrfclaw_vm_semantic_state_t *out)
{
    if (!out || !m_valid ||
        m_state.capability_id != capability_id ||
        m_state.channel != channel)
        return false;
    *out = m_state;
    return true;
}
