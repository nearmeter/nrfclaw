#ifndef NRFCLAW_HA_ROLE_H
#define NRFCLAW_HA_ROLE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRFCLAW_HA_ROLE_NONE = 0,
    NRFCLAW_HA_ROLE_DIRECT_BLE = 1,
    NRFCLAW_HA_ROLE_NINALINK_NODE = 2,
    NRFCLAW_HA_ROLE_BRIDGE = 3
} nrfclaw_ha_role_t;

/* B7.6f persistent Home Assistant transport role.
 * Fresh/factory-reset devices default to NONE: Application/NDP does not
 * advertise until HA integration is explicitly enabled. P0.21/NUS remains
 * available regardless of this role. */
void nrfclaw_ha_role_init(void);
nrfclaw_ha_role_t nrfclaw_ha_role_get(void);
uint16_t nrfclaw_ha_role_node_period_get(void);
bool nrfclaw_ha_role_set_persist(nrfclaw_ha_role_t role);
bool nrfclaw_ha_role_apply_runtime(nrfclaw_ha_role_t role);
bool nrfclaw_ha_role_configure(nrfclaw_ha_role_t role);
bool nrfclaw_ha_role_configure_node(uint16_t period_s);
uint8_t nrfclaw_ha_role_store_status(void);

#endif
