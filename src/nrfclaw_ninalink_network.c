#include "nrfclaw_ninalink_network.h"

#include "nrfclaw_state.h"

#define NRFCLAW_STATE_KEY_NINALINK_NETWORK_ID 21U

static uint16_t m_network_id;
static bool m_has_persisted;

void nrfclaw_ninalink_network_init(void)
{
    uint32_t value;

    m_network_id = NRFCLAW_NINALINK_NETWORK_DEFAULT_ID;
    m_has_persisted = false;

    if (nrfclaw_state_get(NRFCLAW_STATE_KEY_NINALINK_NETWORK_ID, &value)) {
        m_network_id = (uint16_t)(value & 0xFFFFU);
        m_has_persisted = true;
    }
}

uint16_t nrfclaw_ninalink_network_id(void)
{
    return m_network_id;
}

bool nrfclaw_ninalink_network_has_persisted(void)
{
    return m_has_persisted;
}

uint8_t nrfclaw_ninalink_network_store_status(void)
{
    return (uint8_t)nrfclaw_state_status();
}

bool nrfclaw_ninalink_network_set_persist(uint16_t network_id)
{
    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_NINALINK_NETWORK_ID,
            (uint32_t)network_id))
        return false;

    m_network_id = network_id;
    m_has_persisted = true;
    return true;
}
