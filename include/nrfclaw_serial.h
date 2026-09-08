#ifndef NRFCLAW_SERIAL_H
#define NRFCLAW_SERIAL_H
#include <stdbool.h>
#include <stdint.h>
#define NRFCLAW_SERIAL_DEFAULT_BAUD 57600UL
#define NRFCLAW_SERIAL_RX_BUFFER_SIZE 64U

typedef enum {
    NRFCLAW_SERIAL_FRAME_IDLE = 0,
    NRFCLAW_SERIAL_FRAME_LINE = 1,
    NRFCLAW_SERIAL_FRAME_LENGTH = 2
} nrfclaw_serial_frame_mode_t;
void nrfclaw_serial_init(void);
bool nrfclaw_serial_enable(uint8_t tx_pin,uint8_t rx_pin,uint32_t baud);
bool nrfclaw_serial_enable_economy(uint8_t tx_pin,uint8_t rx_pin,uint32_t baud);
void nrfclaw_serial_process(void);
bool nrfclaw_serial_economy_mode(void);
void nrfclaw_serial_disable(void);
bool nrfclaw_serial_active(void);
uint8_t nrfclaw_serial_tx_pin(void);
uint8_t nrfclaw_serial_rx_pin(void);
uint32_t nrfclaw_serial_baud(void);
bool nrfclaw_serial_owns_pin(uint8_t pin);
bool nrfclaw_serial_write(uint8_t const *data,uint16_t len);
bool nrfclaw_serial_read_byte(uint8_t *value);
uint8_t nrfclaw_serial_available(void);
/* Arm one asynchronous frame. param=idle ms, ignored for LINE, or fixed byte count. */
bool nrfclaw_serial_frame_arm(nrfclaw_serial_frame_mode_t mode,uint16_t param);
bool nrfclaw_serial_frame_take(uint8_t *dst,uint8_t max_len,uint8_t *out_len);
void nrfclaw_serial_frame_cancel(void);
#endif
