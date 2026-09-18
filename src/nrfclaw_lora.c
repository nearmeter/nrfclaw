#include "nrfclaw_lora.h"
#include "nrfclaw_llcc68_rl.h"
#include "nrfclaw_lora_profile_store.h"
#include "nrfclaw_board.h"

#include "nrf_drv_gpiote.h"
#include "nrf_gpio.h"
#include "SEGGER_RTT.h"
#include <string.h>

#define LORA_MAX_PAYLOAD 64U
#define LORA_DIAG_QUEUE_DEPTH 8U
#define LORA_DIAG_MAX_PAYLOAD 64U

typedef struct {
    uint8_t len;
    uint8_t data[LORA_DIAG_MAX_PAYLOAD];
    int16_t rssi_x2;
    int16_t snr_x4;
} lora_diag_packet_t;

static nrfclaw_lora_profile_t m_profile;
static bool m_initialized;
static bool m_irq_ready;
static bool m_tx_active;
static bool m_cleanup_pending;
static bool m_rx_active;
static bool m_rx_ready;
static bool m_diag_stream;
static uint8_t m_rx_buffer[LORA_MAX_PAYLOAD];
static uint8_t m_rx_len;
static int16_t m_last_rssi_dbm_x2;
static int16_t m_last_snr_db_x4;
static bool m_last_packet_status_valid;

static lora_diag_packet_t m_diag_q[LORA_DIAG_QUEUE_DEPTH];
static uint8_t m_diag_head, m_diag_tail, m_diag_count;
static uint16_t m_diag_dropped;

static void rf_off(void) { nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_clear(P_LORA_TXEN); }
static void rf_rx(void)  { nrf_gpio_pin_clear(P_LORA_TXEN); nrf_gpio_pin_set(P_LORA_RXEN); }
static void rf_tx(void)  { nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_set(P_LORA_TXEN); }
static void dio1_enable(void)  { if (m_irq_ready) nrf_drv_gpiote_in_event_enable(P_LORA_DIO1, true); }
static void dio1_disable(void) { if (m_irq_ready) nrf_drv_gpiote_in_event_disable(P_LORA_DIO1); }

void nrfclaw_lora_init(void)
{
    if (m_initialized) return;
    nrfclaw_lora_profile_default(&m_profile);
    nrfclaw_lora_profile_store_init();
    if (nrfclaw_lora_profile_store_load(&m_profile)) {
        SEGGER_RTT_printf(0, "LORA: restored persisted profile %lu Hz %+d dBm SF%u BW%u CR4/%u sync=0x%02X preamble=%u\r\n",
                          (unsigned long)m_profile.frequency_hz, (int)m_profile.power_dbm,
                          (unsigned)m_profile.sf, (unsigned)m_profile.bw_khz,
                          (unsigned)(4U + m_profile.cr), (unsigned)m_profile.sync_word,
                          (unsigned)m_profile.preamble_symbols);
    }
    if (!nrfclaw_llcc68_rl_init(&m_profile)) {
        SEGGER_RTT_WriteString(0, "LORA R3.8.18 ERROR: LLCC68 init failed\r\n");
        return;
    }
    m_tx_active = m_cleanup_pending = m_rx_active = m_rx_ready = false;
    m_diag_stream = false;
    m_irq_ready = false;
    m_initialized = true;
    SEGGER_RTT_WriteString(0, "LORA R3.8.18: low-power persistent-profile LLCC68 backend ready\r\n");
}

void nrfclaw_lora_irq_ready(void)
{
    m_irq_ready = true;
    dio1_disable();
}

void nrfclaw_lora_sleep(void)
{
    rf_off();
    (void)nrfclaw_llcc68_rl_sleep();
}

bool nrfclaw_lora_send_async(uint8_t const *data, uint8_t len)
{
    if (!data || len == 0U || len > LORA_MAX_PAYLOAD) return false;
    if (!m_initialized) nrfclaw_lora_init();
    if (!m_initialized || m_tx_active || m_cleanup_pending || m_rx_active) return false;

    rf_tx();
    m_tx_active = true;
    dio1_enable();
    if (!nrfclaw_llcc68_rl_start_tx(data, len)) {
        m_tx_active = false;
        dio1_disable();
        rf_off();
        (void)nrfclaw_llcc68_rl_standby();
        (void)nrfclaw_llcc68_rl_sleep();
        return false;
    }
    SEGGER_RTT_printf(0, "LORA R3.8.18: TX start len=%u\r\n", (unsigned)len);
    return true;
}

bool nrfclaw_lora_receive_async(void)
{
    if (!m_initialized) nrfclaw_lora_init();
    if (!m_initialized || m_tx_active || m_cleanup_pending || m_rx_active) return false;
    m_rx_ready = false;
    m_rx_len = 0U;
    m_rx_active = true;
    rf_rx();
    dio1_enable();
    if (!nrfclaw_llcc68_rl_start_rx(true)) {
        m_rx_active = false;
        dio1_disable();
        rf_off();
        (void)nrfclaw_llcc68_rl_standby();
        (void)nrfclaw_llcc68_rl_sleep();
        return false;
    }
    SEGGER_RTT_WriteString(0, "LORA R3.8.18: RX armed\r\n");
    return true;
}

bool nrfclaw_lora_receive_window_async(uint32_t timeout_ms)
{
    if (timeout_ms == 0U)
        return false;
    if (!m_initialized)
        nrfclaw_lora_init();
    if (!m_initialized || m_tx_active || m_cleanup_pending || m_rx_active)
        return false;

    m_rx_ready = false;
    m_rx_len = 0U;
    m_rx_active = true;
    rf_rx();
    dio1_enable();

    if (!nrfclaw_llcc68_rl_start_rx_ms(timeout_ms)) {
        m_rx_active = false;
        dio1_disable();
        rf_off();
        (void)nrfclaw_llcc68_rl_standby();
        (void)nrfclaw_llcc68_rl_sleep();
        return false;
    }

    SEGGER_RTT_printf(
        0,
        "LORA B4.3: timed RX armed %lu ms\r\n",
        (unsigned long)timeout_ms);
    return true;
}

bool nrfclaw_lora_idle(void)
{
    return m_initialized &&
           !m_tx_active &&
           !m_cleanup_pending &&
           !m_rx_active;
}

bool nrfclaw_lora_cancel_receive(void)
{
    m_diag_stream = false;
    m_rx_active = false;
    m_rx_ready = false;
    m_rx_len = 0U;
    dio1_disable();
    rf_off();
    (void)nrfclaw_llcc68_rl_finish_rx();
    return true;
}

bool nrfclaw_lora_diag_stream_start(void)
{
    if (m_tx_active || m_cleanup_pending || m_rx_active) return false;
    m_diag_head = m_diag_tail = m_diag_count = 0U;
    m_diag_dropped = 0U;
    m_diag_stream = true;
    if (!nrfclaw_lora_receive_async()) { m_diag_stream = false; return false; }
    SEGGER_RTT_WriteString(0, "LORA R3.8.18: diagnostic RX stream active\r\n");
    return true;
}

bool nrfclaw_lora_diag_stream_take(uint8_t *data, uint8_t *len, uint8_t max_len,
                                    int16_t *rssi_dbm_x2, int16_t *snr_db_x4)
{
    if (!data || !len || !rssi_dbm_x2 || !snr_db_x4 || m_diag_count == 0U) return false;
    lora_diag_packet_t const *p = &m_diag_q[m_diag_tail];
    if (p->len == 0U || p->len > max_len) return false;
    memcpy(data, p->data, p->len); *len = p->len;
    *rssi_dbm_x2 = p->rssi_x2; *snr_db_x4 = p->snr_x4;
    m_diag_tail = (uint8_t)((m_diag_tail + 1U) % LORA_DIAG_QUEUE_DEPTH);
    m_diag_count--;
    return true;
}

bool nrfclaw_lora_diag_stream_active(void) { return m_diag_stream && m_rx_active; }
uint16_t nrfclaw_lora_diag_stream_dropped(void) { return m_diag_dropped; }
bool nrfclaw_lora_rx_active(void) { return m_rx_active; }
bool nrfclaw_lora_rx_ready(void) { return m_rx_ready; }

bool nrfclaw_lora_get_last_packet_status(int16_t *rssi_dbm_x2, int16_t *snr_db_x4)
{
    if (!rssi_dbm_x2 || !snr_db_x4 || !m_last_packet_status_valid) return false;
    *rssi_dbm_x2 = m_last_rssi_dbm_x2; *snr_db_x4 = m_last_snr_db_x4; return true;
}

bool nrfclaw_lora_take_rx(uint8_t *data, uint8_t *len, uint8_t max_len)
{
    if (!data || !len || !m_rx_ready || m_rx_len == 0U || m_rx_len > max_len) return false;
    memcpy(data, m_rx_buffer, m_rx_len); *len = m_rx_len;
    m_rx_ready = false; m_rx_len = 0U; return true;
}

static void queue_diag_packet(void)
{
    if (m_rx_len == 0U || m_rx_len > LORA_DIAG_MAX_PAYLOAD) { m_diag_dropped++; return; }
    if (m_diag_count >= LORA_DIAG_QUEUE_DEPTH) { m_diag_dropped++; return; }
    lora_diag_packet_t *p = &m_diag_q[m_diag_head];
    p->len = m_rx_len; memcpy(p->data, m_rx_buffer, m_rx_len);
    p->rssi_x2 = m_last_rssi_dbm_x2; p->snr_x4 = m_last_snr_db_x4;
    m_diag_head = (uint8_t)((m_diag_head + 1U) % LORA_DIAG_QUEUE_DEPTH);
    m_diag_count++;
}

bool nrfclaw_lora_on_dio1_event(void)
{
    uint16_t irq = 0U;
    if (!nrfclaw_llcc68_rl_get_irq(&irq)) return false;

    if (m_rx_active) {
        bool valid = (irq & NRFCLAW_LLCC68_IRQ_RX_DONE) != 0U &&
                     (irq & (NRFCLAW_LLCC68_IRQ_CRC_ERR | NRFCLAW_LLCC68_IRQ_HEADER_ERR)) == 0U;
        bool packet_ok = false;
        if (valid) {
            uint8_t n = sizeof(m_rx_buffer);
            packet_ok = nrfclaw_llcc68_rl_read_packet(m_rx_buffer, &n, sizeof(m_rx_buffer),
                                                       &m_last_rssi_dbm_x2, &m_last_snr_db_x4);
            if (packet_ok) {
                m_rx_len = n; m_rx_ready = true; m_last_packet_status_valid = true;
                SEGGER_RTT_printf(0, "LORA R3.8.18 RX_DONE len=%u RSSI_x2=%d SNR_x4=%d\r\n",
                                  (unsigned)n, (int)m_last_rssi_dbm_x2, (int)m_last_snr_db_x4);
            }
        }

        if (m_diag_stream) {
            if (packet_ok) queue_diag_packet();
            m_rx_ready = false; m_rx_len = 0U;

            /* Core R3.8.17 fix: RadioLib-like explicit receive re-stage after
             * every completion/error. This avoids the one-packet RX state that
             * was observed with the old driver. */
            if (!nrfclaw_llcc68_rl_rearm_rx()) {
                SEGGER_RTT_WriteString(0, "LORA R3.8.18 ERROR: RX rearm failed\r\n");
                m_rx_active = false; m_diag_stream = false; dio1_disable(); rf_off();
            } else {
                /* The GPIOTE ISR intentionally disables DIO1 before queuing the
                 * event.  Continuous RX must explicitly re-enable the GPIO IRQ
                 * after the LLCC68 IRQ has been cleared and SetRx() has been
                 * issued again, otherwise only the first packet is observable. */
                dio1_enable();
                SEGGER_RTT_WriteString(0, "LORA R3.8.18: RX rearmed, DIO1 enabled\r\n");
            }
            return false;
        }

        m_rx_active = false;
        dio1_disable(); rf_off();
        (void)nrfclaw_llcc68_rl_finish_rx();
        return false;
    }

    if (!m_tx_active) {
        (void)nrfclaw_llcc68_rl_clear_irq(irq ? irq : 0xFFFFU);
        return false;
    }
    if ((irq & NRFCLAW_LLCC68_IRQ_TX_DONE) != 0U) {
        m_tx_active = false;
        rf_off();
        m_cleanup_pending = true;
        SEGGER_RTT_WriteString(0, "LORA R3.8.18: TX_DONE\r\n");
        return true;
    }
    if ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT) != 0U) {
        /* Terminal TX failure must follow the same ClearIRQ/Standby/Sleep path. */
        m_tx_active = false;
        rf_off();
        m_cleanup_pending = true;
        SEGGER_RTT_WriteString(0, "LORA R3.8.18: TX timeout -> cleanup\r\n");
    }
    return false;
}

void nrfclaw_lora_process(void)
{
    if (!m_cleanup_pending || m_tx_active) return;
    if (!nrfclaw_llcc68_rl_finish_tx()) return;
    m_cleanup_pending = false;
    dio1_disable(); rf_off();
}

bool nrfclaw_lora_set_profile(nrfclaw_lora_profile_t const *profile)
{
    if (!nrfclaw_lora_profile_validate(profile) || m_tx_active || m_cleanup_pending || m_rx_active) return false;
    if (!m_initialized) nrfclaw_lora_init();
    if (!m_initialized) return false;
    if (!nrfclaw_llcc68_rl_apply_profile(profile)) return false;
    m_profile = *profile;
    if (!nrfclaw_llcc68_rl_sleep()) return false;
    return true;
}

bool nrfclaw_lora_set_profile_persist(nrfclaw_lora_profile_t const *profile)
{
    if (!nrfclaw_lora_set_profile(profile)) return false;
    return nrfclaw_lora_profile_store_request_save(profile);
}

void nrfclaw_lora_profile_process(void)
{
    nrfclaw_lora_profile_store_process();
}

void nrfclaw_lora_get_profile(nrfclaw_lora_profile_t *profile) { if (profile) *profile = m_profile; }

bool nrfclaw_lora_set_frequency(uint32_t frequency_hz)
{
    nrfclaw_lora_profile_t p = m_profile;
    if (!m_initialized) nrfclaw_lora_profile_default(&p);
    p.frequency_hz = frequency_hz;
    return nrfclaw_lora_set_profile(&p);
}
