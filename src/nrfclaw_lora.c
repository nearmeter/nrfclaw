#include "nrfclaw_lora.h"
#include "nrfclaw_llcc68_rl.h"
#include "nrfclaw_lora_profile_store.h"
#include "nrfclaw_board.h"

#include "nrf_drv_gpiote.h"
#include "nrf_gpio.h"
#include "app_timer.h"
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
static nrfclaw_lora_rx_diag_t m_rx_diag;
static nrfclaw_lora_tx_diag_t m_tx_diag;
static uint8_t m_tx_diag_current_len;
static nrfclaw_lora_turnaround_timing_t m_turn_timing;
static bool m_turn_timed_rx_active;


#define TURN_VALID_RX18_DONE       0x01U
#define TURN_VALID_TX64_START      0x02U
#define TURN_VALID_TX64_DONE       0x04U
#define TURN_VALID_TX18_DONE       0x08U
#define TURN_VALID_TIMED_RX_START  0x10U
#define TURN_VALID_TIMED_RX_TERM   0x20U
#define TURN_RTC_MASK              0x00FFFFFFUL

static uint32_t turn_tick_now(void)
{
    return app_timer_cnt_get();
}

static uint32_t turn_tick_diff(uint32_t newer, uint32_t older)
{
    return (newer - older) & TURN_RTC_MASK;
}

static void turn_timing_reset(void)
{
    memset(&m_turn_timing, 0, sizeof(m_turn_timing));
    m_turn_timing.ticks_per_second = APP_TIMER_TICKS(1000U);
    m_turn_timed_rx_active = false;
}

static void sat_inc_u16(uint16_t *v)
{
    if (v && *v != UINT16_MAX)
        (*v)++;
}

static void rf_off(void) { nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_clear(P_LORA_TXEN); }
static void rf_rx(void)  { nrf_gpio_pin_clear(P_LORA_TXEN); nrf_gpio_pin_set(P_LORA_RXEN); }
static void rf_tx(void)  { nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_set(P_LORA_TXEN); }
static void dio1_enable(void)  { if (m_irq_ready) nrf_drv_gpiote_in_event_enable(P_LORA_DIO1, true); }
static void dio1_disable(void) { if (m_irq_ready) nrf_drv_gpiote_in_event_disable(P_LORA_DIO1); }

void nrfclaw_lora_init(void)
{
    if (m_initialized) return;
    nrfclaw_lora_profile_default(&m_profile);
#if !NRFCLAW_BOARD_HAS_LORA
    ((void)0);
    return;
#endif
    nrfclaw_lora_profile_store_init();
    if (nrfclaw_lora_profile_store_load(&m_profile)) {
        ((void)0);
    }
    if (!nrfclaw_llcc68_rl_init(&m_profile)) {
        ((void)0);
        return;
    }
    m_tx_active = m_cleanup_pending = m_rx_active = m_rx_ready = false;
    m_diag_stream = false;
    memset(&m_rx_diag, 0, sizeof(m_rx_diag));
    memset(&m_tx_diag, 0, sizeof(m_tx_diag));
    m_tx_diag_current_len = 0U;
    turn_timing_reset();
    m_irq_ready = false;
    m_initialized = true;
    ((void)0);
}

void nrfclaw_lora_irq_ready(void)
{
    m_irq_ready = true;
    dio1_disable();
}

void nrfclaw_lora_sleep(void)
{
    /* Boards without LoRa never initialize the radio. Do not touch their
     * inherited placeholder pin map during minimum-power transitions. */
    if (!m_initialized) return;
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
    m_tx_diag_current_len = len;
    m_tx_diag.last_len = len;
    m_tx_diag.last_terminal = 0U;
    m_tx_diag.last_irq = 0U;
    dio1_enable();
    if (!nrfclaw_llcc68_rl_start_tx(data, len)) {
        sat_inc_u16(&m_tx_diag.start_fail);
        m_tx_diag.last_terminal = 3U;
        m_tx_active = false;
        dio1_disable();
        rf_off();
        (void)nrfclaw_llcc68_rl_standby();
        (void)nrfclaw_llcc68_rl_sleep();
        return false;
    }
    {
        uint32_t now = turn_tick_now();
        if (len == 64U) {
            m_turn_timing.tx64_start_tick = now;
            m_turn_timing.valid_flags |= TURN_VALID_TX64_START;
            sat_inc_u16(&m_turn_timing.tx64_count);
            if ((m_turn_timing.valid_flags & TURN_VALID_RX18_DONE) != 0U)
                m_turn_timing.rx18_to_tx64_ticks =
                    turn_tick_diff(now, m_turn_timing.rx18_done_tick);
        }
    }
    sat_inc_u16(&m_tx_diag.start_ok);
    if (len == 30U) sat_inc_u16(&m_tx_diag.len30_start);
    else if (len == 64U) sat_inc_u16(&m_tx_diag.len64_start);
    ((void)0);
    return true;
}

bool nrfclaw_lora_receive_async(void)
{
    if (!m_initialized) nrfclaw_lora_init();
    if (!m_initialized || m_tx_active || m_cleanup_pending || m_rx_active) return false;
    m_rx_ready = false;
    m_rx_len = 0U;
    m_rx_active = true;
    m_turn_timed_rx_active = false;
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
    ((void)0);
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
    m_turn_timed_rx_active = true;
    rf_rx();
    dio1_enable();

    if (!nrfclaw_llcc68_rl_start_rx_ms(timeout_ms)) {
        m_turn_timed_rx_active = false;
        m_rx_active = false;
        dio1_disable();
        rf_off();
        (void)nrfclaw_llcc68_rl_standby();
        (void)nrfclaw_llcc68_rl_sleep();
        return false;
    }

    {
        uint32_t now = turn_tick_now();
        m_turn_timing.timed_rx_start_tick = now;
        m_turn_timing.timed_rx_requested_ms = timeout_ms;
        m_turn_timing.timed_rx_terminal = 0U;
        m_turn_timing.valid_flags |= TURN_VALID_TIMED_RX_START;
        m_turn_timing.valid_flags &= (uint8_t)~TURN_VALID_TIMED_RX_TERM;
        sat_inc_u16(&m_turn_timing.timed_rx_count);
        if ((m_turn_timing.valid_flags & TURN_VALID_TX18_DONE) != 0U)
            m_turn_timing.tx18_to_timed_rx_ticks =
                turn_tick_diff(now, m_turn_timing.tx18_done_tick);
    }

    ((void)0);
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
    m_turn_timed_rx_active = false;
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
    ((void)0);
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

void nrfclaw_lora_rx_diag_get(nrfclaw_lora_rx_diag_t *out)
{
    if (out)
        *out = m_rx_diag;
}

void nrfclaw_lora_rx_diag_clear(void)
{
    memset(&m_rx_diag, 0, sizeof(m_rx_diag));
}

void nrfclaw_lora_tx_diag_get(nrfclaw_lora_tx_diag_t *out)
{
    if (out) *out = m_tx_diag;
}

void nrfclaw_lora_tx_diag_clear(void)
{
    memset(&m_tx_diag, 0, sizeof(m_tx_diag));
    m_tx_diag_current_len = 0U;
}

void nrfclaw_lora_turnaround_timing_get(nrfclaw_lora_turnaround_timing_t *out)
{
    if (out)
        *out = m_turn_timing;
}

void nrfclaw_lora_turnaround_timing_clear(void)
{
    turn_timing_reset();
}

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
    uint32_t event_tick;
    if (!nrfclaw_llcc68_rl_get_irq(&irq)) return false;
    event_tick = turn_tick_now();

    if (m_rx_active) {
        bool valid;
        bool packet_ok = false;

        sat_inc_u16(&m_rx_diag.dio1_events);
        if ((irq & NRFCLAW_LLCC68_IRQ_RX_DONE) != 0U)
            sat_inc_u16(&m_rx_diag.irq_rx_done);
        if ((irq & NRFCLAW_LLCC68_IRQ_CRC_ERR) != 0U)
            sat_inc_u16(&m_rx_diag.irq_crc_err);
        if ((irq & NRFCLAW_LLCC68_IRQ_HEADER_ERR) != 0U)
            sat_inc_u16(&m_rx_diag.irq_header_err);
        if ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT) != 0U)
            sat_inc_u16(&m_rx_diag.irq_timeout);

        valid = (irq & NRFCLAW_LLCC68_IRQ_RX_DONE) != 0U &&
                (irq & (NRFCLAW_LLCC68_IRQ_CRC_ERR | NRFCLAW_LLCC68_IRQ_HEADER_ERR)) == 0U;
        if (valid) {
            uint8_t n = sizeof(m_rx_buffer);
            packet_ok = nrfclaw_llcc68_rl_read_packet(m_rx_buffer, &n, sizeof(m_rx_buffer),
                                                       &m_last_rssi_dbm_x2, &m_last_snr_db_x4);
            if (packet_ok) {
                sat_inc_u16(&m_rx_diag.packet_read_ok);
                m_rx_diag.last_len = n;
                if (n == 18U) {
                    m_turn_timing.rx18_done_tick = event_tick;
                    m_turn_timing.valid_flags |= TURN_VALID_RX18_DONE;
                    sat_inc_u16(&m_turn_timing.rx18_count);
                }
                m_rx_len = n; m_rx_ready = true; m_last_packet_status_valid = true;
                ((void)0);
            } else {
                sat_inc_u16(&m_rx_diag.packet_read_fail);
                m_rx_diag.last_len = 0U;
            }
        }

        if (m_turn_timed_rx_active) {
            uint8_t terminal = 0U;
            if ((irq & NRFCLAW_LLCC68_IRQ_RX_DONE) != 0U)
                terminal = 1U;
            else if ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT) != 0U)
                terminal = 2U;
            else if (irq != 0U)
                terminal = 3U;

            if (terminal != 0U) {
                m_turn_timing.timed_rx_terminal_tick = event_tick;
                m_turn_timing.timed_rx_terminal = terminal;
                m_turn_timing.valid_flags |= TURN_VALID_TIMED_RX_TERM;
                if ((m_turn_timing.valid_flags & TURN_VALID_TIMED_RX_START) != 0U)
                    m_turn_timing.timed_rx_span_ticks =
                        turn_tick_diff(event_tick, m_turn_timing.timed_rx_start_tick);
                m_turn_timed_rx_active = false;
            }
        }

        if (m_diag_stream) {
            if (packet_ok) queue_diag_packet();
            m_rx_ready = false; m_rx_len = 0U;

            /* Core R3.8.17 fix: RadioLib-like explicit receive re-stage after
             * every completion/error. This avoids the one-packet RX state that
             * was observed with the old driver. */
            if (!nrfclaw_llcc68_rl_rearm_rx()) {
                ((void)0);
                m_rx_active = false; m_diag_stream = false; dio1_disable(); rf_off();
            } else {
                /* The GPIOTE ISR intentionally disables DIO1 before queuing the
                 * event.  Continuous RX must explicitly re-enable the GPIO IRQ
                 * after the LLCC68 IRQ has been cleared and SetRx() has been
                 * issued again, otherwise only the first packet is observable. */
                dio1_enable();
                ((void)0);
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
        if (m_tx_diag_current_len == 18U) {
            m_turn_timing.tx18_done_tick = event_tick;
            m_turn_timing.valid_flags |= TURN_VALID_TX18_DONE;
            sat_inc_u16(&m_turn_timing.tx18_count);
        } else if (m_tx_diag_current_len == 64U) {
            m_turn_timing.tx64_done_tick = event_tick;
            m_turn_timing.valid_flags |= TURN_VALID_TX64_DONE;
            if ((m_turn_timing.valid_flags & TURN_VALID_TX64_START) != 0U)
                m_turn_timing.tx64_air_ticks =
                    turn_tick_diff(event_tick, m_turn_timing.tx64_start_tick);
        }
        sat_inc_u16(&m_tx_diag.tx_done);
        if (m_tx_diag_current_len == 30U) sat_inc_u16(&m_tx_diag.len30_done);
        else if (m_tx_diag_current_len == 64U) sat_inc_u16(&m_tx_diag.len64_done);
        m_tx_diag.last_len = m_tx_diag_current_len;
        m_tx_diag.last_terminal = 1U;
        m_tx_diag.last_irq = irq;
        m_tx_active = false;
        rf_off();
        m_cleanup_pending = true;
        ((void)0);
        return true;
    }
    if ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT) != 0U) {
        sat_inc_u16(&m_tx_diag.tx_timeout);
        if (m_tx_diag_current_len == 30U) sat_inc_u16(&m_tx_diag.len30_timeout);
        else if (m_tx_diag_current_len == 64U) sat_inc_u16(&m_tx_diag.len64_timeout);
        m_tx_diag.last_len = m_tx_diag_current_len;
        m_tx_diag.last_terminal = 2U;
        m_tx_diag.last_irq = irq;
        /* Terminal TX failure must follow the same ClearIRQ/Standby/Sleep path. */
        m_tx_active = false;
        rf_off();
        m_cleanup_pending = true;
        ((void)0);
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
