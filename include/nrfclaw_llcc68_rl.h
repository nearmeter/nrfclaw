#ifndef NRFCLAW_LLCC68_RL_H
#define NRFCLAW_LLCC68_RL_H

#include <stdint.h>
#include <stddef.h>
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

/* B8.0f GFSK RAW RX -- experimental physical-NUS-only feasibility path. */
/* B8.1g generic LLCC68/SX126x GFSK RX-bandwidth codes. */
#define NRFCLAW_LLCC68_GFSK_BW_4_8    0x1FU
#define NRFCLAW_LLCC68_GFSK_BW_5_8    0x17U
#define NRFCLAW_LLCC68_GFSK_BW_7_3    0x0FU
#define NRFCLAW_LLCC68_GFSK_BW_9_7    0x1EU
#define NRFCLAW_LLCC68_GFSK_BW_11_7   0x16U
#define NRFCLAW_LLCC68_GFSK_BW_14_6   0x0EU
#define NRFCLAW_LLCC68_GFSK_BW_19_5   0x1DU
#define NRFCLAW_LLCC68_GFSK_BW_23_4   0x15U
#define NRFCLAW_LLCC68_GFSK_BW_29_3   0x0DU
#define NRFCLAW_LLCC68_GFSK_BW_39_0   0x1CU
#define NRFCLAW_LLCC68_GFSK_BW_46_9   0x14U
#define NRFCLAW_LLCC68_GFSK_BW_58_6   0x0CU
#define NRFCLAW_LLCC68_GFSK_BW_78_2   0x1BU
#define NRFCLAW_LLCC68_GFSK_BW_93_8   0x13U
#define NRFCLAW_LLCC68_GFSK_BW_117_3  0x0BU
#define NRFCLAW_LLCC68_GFSK_BW_156_2  0x1AU
#define NRFCLAW_LLCC68_GFSK_BW_187_2  0x12U
#define NRFCLAW_LLCC68_GFSK_BW_234_3  0x0AU
#define NRFCLAW_LLCC68_GFSK_BW_312_0  0x19U
#define NRFCLAW_LLCC68_GFSK_BW_373_6  0x11U
#define NRFCLAW_LLCC68_GFSK_BW_467_0  0x09U
typedef enum {
    NRFCLAW_LLCC68_GFSK_PACKET_FIXED = 0U,
    NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE = 1U
} nrfclaw_llcc68_gfsk_packet_mode_t;

typedef struct {
    uint32_t frequency_hz;
    uint32_t bitrate_bps;
    uint32_t deviation_hz;
    uint8_t rx_bw_code;
    uint16_t preamble_bits;
    uint8_t sync[4];
    uint8_t sync_len;
    uint8_t payload_len;
    uint8_t packet_mode;
} nrfclaw_llcc68_gfsk_profile_t;

bool nrfclaw_llcc68_rl_gfsk_start_profile(
    nrfclaw_llcc68_gfsk_profile_t const *profile);

bool nrfclaw_llcc68_rl_gfsk_start_vevor(uint32_t frequency_hz,uint8_t rx_bw_code);
bool nrfclaw_llcc68_rl_gfsk_poll_ex(
    uint16_t *irq, uint8_t *data, size_t *len, size_t max_len,
    bool *packet_ready);

bool nrfclaw_llcc68_rl_gfsk_poll(uint16_t *irq,uint8_t *data,uint8_t *len,uint8_t max_len,bool *packet_ready);

/* B8.1k7d3 diagnostic RX-bandwidth sweep. */
bool nrfclaw_llcc68_rl_gfsk_stop_restore_lora(void);
bool nrfclaw_llcc68_rl_gfsk_tx_vevor(uint32_t frequency_hz,
                                      int8_t power_dbm,
                                      uint16_t *terminal_irq);
bool nrfclaw_llcc68_rl_gfsk_tx_profile(
    nrfclaw_llcc68_gfsk_profile_t const *profile,
    uint8_t const *payload,
    uint8_t payload_len,
    int8_t power_dbm,
    uint16_t *terminal_irq);

bool nrfclaw_llcc68_rl_gfsk_tx_vevor_variant(uint32_t frequency_hz,
                                              int8_t power_dbm,
                                              uint8_t variant,
                                              uint16_t *terminal_irq);
bool nrfclaw_llcc68_rl_gfsk_active(void);


bool nrfclaw_llcc68_rl_get_irq(uint16_t *irq);
bool nrfclaw_llcc68_rl_clear_irq(uint16_t mask);
bool nrfclaw_llcc68_rl_read_packet(uint8_t *data, uint8_t *len, uint8_t max_len,
                                    int16_t *rssi_dbm_x2, int16_t *snr_db_x4);

#endif
