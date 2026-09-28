#ifndef NRFCLAW_NINALINK_NETWORK_H
#define NRFCLAW_NINALINK_NETWORK_H

#include <stdbool.h>
#include <stdint.h>

/*
 * B7.6f2l1 — NinaLink Network ID Isolation.
 *
 * The NinaLink v1 wire format has always reserved bytes 4..5 for network_id.
 * This module makes that existing field configurable and persistent without
 * changing frame length, LoRa PHY settings, ACK semantics or discovery.
 */
#define NRFCLAW_NINALINK_NETWORK_DEFAULT_ID 0x0000U

void nrfclaw_ninalink_network_init(void);
uint16_t nrfclaw_ninalink_network_id(void);
bool nrfclaw_ninalink_network_has_persisted(void);
uint8_t nrfclaw_ninalink_network_store_status(void);
bool nrfclaw_ninalink_network_set_persist(uint16_t network_id);

#endif
