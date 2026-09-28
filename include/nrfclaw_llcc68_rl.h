#ifndef NRFCLAW_LLCC68_RL_H
#define NRFCLAW_LLCC68_RL_H

#include <stdint.h>
#include <stdbool.h>
#include "nrfclaw_lora_profile.h"

/*
 * Small C LLCC68 backend for nRFClaw.
 *
 * The transaction ordering and LLCC68/SX126x operating model are derived from
 * RadioLib (jgromes/RadioLib, MIT license), but this is not a copy of the C++
 * RadioLib class hierarchy.  It is an nRF5 SDK C implementation that keeps the
 * nRFClaw GPIO/SPI/event model.
 */

typedef enum {
    NRFCLAW_LLCC68_IRQ_NONE       = 0x0000,
    NRFCLAW_LLCC68_IRQ_TX_DONE    = 0x0001,
    NRFCLAW_LLCC68_IRQ_RX_DONE    = 0x0002,
    NRFCLAW_LLCC68_IRQ_PREAMBLE   = 0x0004,
    NRFCLAW_LLCC68_IRQ_HEADER_OK  = 0x0010,
    NRFCLAW_LLCC68_IRQ_HEADER_ERR = 0x0020,
    NRFCLAW_LLCC68_IRQ_CRC_ERR    = 0x0040,
    NRFCLAW_LLCC68_IRQ_TIMEOUT    = 0x0200
} nrfclaw_llcc68_irq_t;

bool nrfclaw_llcc68_rl_init(nrfclaw_lora_profile_t const *profile);
bool nrfclaw_llcc68_rl_apply_profile(nrfclaw_lora_profile_t const *profile);
bool nrfclaw_llcc68_rl_wakeup(void);
bool nrfclaw_llcc68_rl_standby(void);
bool nrfclaw_llcc68_rl_sleep(void);

bool nrfclaw_llcc68_rl_start_tx(uint8_t const *data, uint8_t len);
bool nrfclaw_llcc68_rl_finish_tx(void);

/* Start one receive window. infinite=true uses SX126x RxContinuous. */
bool nrfclaw_llcc68_rl_start_rx(bool infinite);
bool nrfclaw_llcc68_rl_start_rx_ms(uint32_t timeout_ms);
bool nrfclaw_llcc68_rl_rearm_rx(void);
bool nrfclaw_llcc68_rl_finish_rx(void);

bool nrfclaw_llcc68_rl_get_irq(uint16_t *irq);
bool nrfclaw_llcc68_rl_clear_irq(uint16_t mask);
bool nrfclaw_llcc68_rl_read_packet(uint8_t *data, uint8_t *len, uint8_t max_len,
                                    int16_t *rssi_dbm_x2, int16_t *snr_db_x4);

#endif
