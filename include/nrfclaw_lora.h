#ifndef NRFCLAW_LORA_H
#define NRFCLAW_LORA_H

#include <stdint.h>
#include <stdbool.h>
#include "nrfclaw_lora_profile.h"


void nrfclaw_lora_init(void);

void nrfclaw_lora_sleep(void);
bool nrfclaw_lora_set_profile(nrfclaw_lora_profile_t const *profile);
/* User/NDP configuration path: apply now and atomically persist for next boot. */
bool nrfclaw_lora_set_profile_persist(nrfclaw_lora_profile_t const *profile);
void nrfclaw_lora_get_profile(nrfclaw_lora_profile_t *profile);
bool nrfclaw_lora_set_frequency(uint32_t frequency_hz);
/* Deferred SoftDevice flash writer for the persisted RF profile. */
void nrfclaw_lora_profile_process(void);


/*
 * Called after main.c has created the GPIOTE DIO1 channel.
 */
void nrfclaw_lora_irq_ready(void);


/*
 * Starts an asynchronous LoRa transmission.
 *
 * Returns immediately after SetTx().
 *
 * TX completion is delivered asynchronously through:
 *
 * DIO1
 *   ->
 * NRFCLAW_EVT_LORA_DIO1
 *   ->
 * NRFCLAW_EVT_LORA_TX_DONE
 */
bool nrfclaw_lora_send_async(
    uint8_t const *data,
    uint8_t len
);


/*
 * Called from main context when the DIO1 ISR generated an event.
 */
bool nrfclaw_lora_on_dio1_event(void);

/* R3.8.16a receive path. A single packet is captured asynchronously into
 * an internal radio buffer and can then be copied into the VM scratch buffer. */
bool nrfclaw_lora_receive_async(void);
bool nrfclaw_lora_receive_window_async(uint32_t timeout_ms);
bool nrfclaw_lora_cancel_receive(void);
bool nrfclaw_lora_take_rx(uint8_t *data, uint8_t *len, uint8_t max_len);
bool nrfclaw_lora_rx_active(void);
bool nrfclaw_lora_rx_ready(void);
bool nrfclaw_lora_idle(void);
bool nrfclaw_lora_get_last_packet_status(int16_t *rssi_dbm_x2, int16_t *snr_db_x4);

/* R3.8.16b1n direct diagnostic stream.  This path is intentionally separate
 * from the VM single-packet RX primitive: packets are queued in firmware and
 * the LLCC68 remains in continuous RX while the NUS CLI drains the queue. */
bool nrfclaw_lora_diag_stream_start(void);
bool nrfclaw_lora_diag_stream_take(uint8_t *data, uint8_t *len, uint8_t max_len,
                                    int16_t *rssi_dbm_x2, int16_t *snr_db_x4);
bool nrfclaw_lora_diag_stream_active(void);
uint16_t nrfclaw_lora_diag_stream_dropped(void);

/* B7.6f2a RAM-only timed-RX diagnostics. No radio behavior changes. */
typedef struct {
    uint16_t dio1_events;
    uint16_t irq_rx_done;
    uint16_t irq_crc_err;
    uint16_t irq_header_err;
    uint16_t irq_timeout;
    uint16_t packet_read_ok;
    uint16_t packet_read_fail;
    uint8_t last_len;
} nrfclaw_lora_rx_diag_t;

void nrfclaw_lora_rx_diag_get(nrfclaw_lora_rx_diag_t *out);
void nrfclaw_lora_rx_diag_clear(void);

/* B7.6f2b RAM-only TX terminal diagnostics. No RF behavior changes. */
typedef struct {
    uint16_t start_ok;
    uint16_t start_fail;
    uint16_t tx_done;
    uint16_t tx_timeout;
    uint16_t len30_start;
    uint16_t len30_done;
    uint16_t len30_timeout;
    uint16_t len64_start;
    uint16_t len64_done;
    uint16_t len64_timeout;
    uint8_t last_len;
    uint8_t last_terminal; /* 0=start/none, 1=TX_DONE, 2=TX_TIMEOUT, 3=START_FAIL */
    uint16_t last_irq;
} nrfclaw_lora_tx_diag_t;

void nrfclaw_lora_tx_diag_get(nrfclaw_lora_tx_diag_t *out);
void nrfclaw_lora_tx_diag_clear(void);

/* B7.6f2c RAM-only turnaround timing correlation. No RF timing changes. */
typedef struct {
    uint32_t ticks_per_second;
    uint32_t rx18_done_tick;
    uint32_t tx64_start_tick;
    uint32_t tx64_done_tick;
    uint32_t rx18_to_tx64_ticks;
    uint32_t tx64_air_ticks;
    uint32_t tx18_done_tick;
    uint32_t timed_rx_start_tick;
    uint32_t timed_rx_terminal_tick;
    uint32_t tx18_to_timed_rx_ticks;
    uint32_t timed_rx_span_ticks;
    uint32_t timed_rx_requested_ms;
    uint16_t rx18_count;
    uint16_t tx64_count;
    uint16_t tx18_count;
    uint16_t timed_rx_count;
    uint8_t valid_flags;
    uint8_t timed_rx_terminal; /* 0=none, 1=RX_DONE, 2=TIMEOUT, 3=other IRQ */
} nrfclaw_lora_turnaround_timing_t;

void nrfclaw_lora_turnaround_timing_get(nrfclaw_lora_turnaround_timing_t *out);
void nrfclaw_lora_turnaround_timing_clear(void);

/*
 * Deferred radio housekeeping.
 *
 * Clears LLCC68 IRQ and returns radio to SLEEP.
 */
void nrfclaw_lora_process(void);


#endif
