#include "nrfclaw_llcc68_rl.h"
#include "nrfclaw_board.h"

#include "nrf_drv_spi.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "app_error.h"
#include <string.h>

/*
 * R3.8.17 RadioLib-derived LLCC68 backend
 * ---------------------------------------
 * RadioLib reference: jgromes/RadioLib, SX126x/SX1262/LLCC68 modules (MIT).
 * This file is an original C port of the subset needed by nRFClaw.  In
 * particular, receive is explicitly prepared/re-armed after every packet,
 * mirroring RadioLib's startReceive()/readData()/startReceive() usage instead
 * of assuming that a single SetRx(0xFFFFFF) transaction is sufficient forever.
 */

#define CMD_SET_SLEEP               0x84U
#define CMD_SET_STANDBY             0x80U
#define CMD_SET_TX                  0x83U
#define CMD_SET_RX                  0x82U
#define CMD_SET_PACKET_TYPE         0x8AU
#define CMD_GET_IRQ_STATUS          0x12U
#define CMD_CLEAR_IRQ_STATUS        0x02U
#define CMD_SET_DIO_IRQ_PARAMS      0x08U
#define CMD_GET_RX_BUFFER_STATUS    0x13U
#define CMD_GET_PACKET_STATUS       0x14U
#define CMD_WRITE_BUFFER            0x0EU
#define CMD_READ_BUFFER             0x1EU
#define CMD_WRITE_REGISTER          0x0DU
#define CMD_SET_RF_FREQUENCY        0x86U
#define CMD_SET_TX_PARAMS           0x8EU
#define CMD_SET_MODULATION_PARAMS   0x8BU
#define CMD_SET_PACKET_PARAMS       0x8CU
#define CMD_SET_BUFFER_BASE_ADDRESS 0x8FU
#define CMD_SET_REGULATOR_MODE      0x96U
#define CMD_CALIBRATE_IMAGE         0x98U
#define CMD_SET_PA_CONFIG           0x95U
#define CMD_GET_STATUS              0xC0U

#define PACKET_TYPE_LORA            0x01U
#define STANDBY_RC                  0x00U
#define REGULATOR_DCDC              0x01U
#define HEADER_EXPLICIT             0x00U
#define CRC_ON                      0x01U
#define IQ_NORMAL                   0x00U
#define RAMP_200_US                 0x04U
#define RX_CONTINUOUS               0xFFFFFFUL
#define RX_TIMEOUT_4S               (4000UL * 64UL)
/* RadioLib-compatible LoRa sync word registers. 0x12 -> 0x1424, 0x34 -> 0x3444. */
#define REG_LORA_SYNC_MSB           0x0740U

static const nrf_drv_spi_t m_spi = NRF_DRV_SPI_INSTANCE(0);
static bool m_spi_initialized = false;
static bool m_radio_sleeping = false;
static nrfclaw_lora_profile_t m_profile;

static void cs_low(void)  { nrf_gpio_pin_clear(P_LORA_NSS); }
static void cs_high(void) { nrf_gpio_pin_set(P_LORA_NSS); }
static bool busy(void)    { return nrf_gpio_pin_read(P_LORA_BUSY) != 0U; }

static bool wait_busy(void)
{
    /* ~20 ms worst-case guard without depending on app_timer. */
    for (uint32_t i = 0U; i < 20000U; ++i) {
        if (!busy()) return true;
        nrf_delay_us(1);
    }
    return false;
}

static bool bus_acquire(void)
{
    nrf_gpio_cfg_output(P_LORA_NSS);  nrf_gpio_pin_set(P_LORA_NSS);
    nrf_gpio_cfg_output(P_LORA_NRST); nrf_gpio_pin_set(P_LORA_NRST);
    /* Preserve RF-switch levels selected by nrfclaw_lora.c before wake. */
    nrf_gpio_cfg_output(P_LORA_RXEN);
    nrf_gpio_cfg_output(P_LORA_TXEN);
    nrf_gpio_cfg_input(P_LORA_BUSY, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(P_LORA_DIO2, NRF_GPIO_PIN_NOPULL);

    if (!m_spi_initialized) {
        nrf_drv_spi_config_t cfg = NRF_DRV_SPI_DEFAULT_CONFIG;
        cfg.sck_pin = P_LORA_SCK;
        cfg.mosi_pin = P_LORA_MOSI;
        cfg.miso_pin = P_LORA_MISO;
        cfg.ss_pin = NRF_DRV_SPI_PIN_NOT_USED;
        cfg.frequency = NRF_DRV_SPI_FREQ_2M;
        cfg.mode = NRF_DRV_SPI_MODE_0;
        cfg.irq_priority = APP_IRQ_PRIORITY_LOWEST;
        if (nrf_drv_spi_init(&m_spi, &cfg, NULL, NULL) != NRF_SUCCESS) return false;
        m_spi_initialized = true;
    }
    return true;
}

static void bus_release(void)
{
    /* The module cannot be power-gated on NINASENSE.  After SetSleep, release
     * the MCU side of SPI so no peripheral/HFCLK/GPIO state is kept alive. */
    if (m_spi_initialized) {
        nrf_drv_spi_uninit(&m_spi);
        m_spi_initialized = false;
    }
    nrf_gpio_cfg_input(P_LORA_SCK,  NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(P_LORA_MOSI, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(P_LORA_MISO, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(P_LORA_BUSY, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(P_LORA_DIO2, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_output(P_LORA_NSS);  nrf_gpio_pin_set(P_LORA_NSS);
    nrf_gpio_cfg_output(P_LORA_NRST); nrf_gpio_pin_set(P_LORA_NRST);
    nrf_gpio_cfg_output(P_LORA_RXEN); nrf_gpio_pin_clear(P_LORA_RXEN);
    nrf_gpio_cfg_output(P_LORA_TXEN); nrf_gpio_pin_clear(P_LORA_TXEN);
}

static bool xfer(uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!m_spi_initialized) return false;
    return nrf_drv_spi_transfer(&m_spi, tx, n, rx, n) == NRF_SUCCESS;
}

static bool cmd_write(uint8_t op, uint8_t const *payload, uint8_t len)
{
    uint8_t tx[72] = {0};
    uint8_t rx[72] = {0};
    if (len > 70U || !wait_busy()) return false;
    tx[0] = op;
    if (payload && len) memcpy(&tx[1], payload, len);
    cs_low();
    bool ok = xfer(tx, rx, (size_t)len + 1U);
    cs_high();
    if (!ok) return false;
    if (op == CMD_SET_SLEEP) return true;
    return wait_busy();
}

static bool write_reg(uint16_t address, uint8_t const *data, uint8_t len)
{
    uint8_t p[66] = {0};
    if (!data || len == 0U || len > 64U) return false;
    p[0] = (uint8_t)(address >> 8);
    p[1] = (uint8_t)address;
    memcpy(&p[2], data, len);
    return cmd_write(CMD_WRITE_REGISTER, p, (uint8_t)(len + 2U));
}

static uint8_t bw_code(uint16_t khz)
{
    if (khz == 500U) return 0x06U;
    if (khz == 250U) return 0x05U;
    return 0x04U; /* 125 kHz */
}

static uint8_t ldro(nrfclaw_lora_profile_t const *p)
{
    /* Equivalent criterion to RadioLib: optimize when LoRa symbol >= 16 ms. */
    uint32_t symbol_us = ((1UL << p->sf) * 1000UL) / p->bw_khz;
    return symbol_us >= 16000UL ? 1U : 0U;
}

static bool set_packet_params(uint8_t payload_len)
{
    uint16_t preamble = m_profile.preamble_symbols;
    uint8_t p[6] = {
        (uint8_t)(preamble >> 8), (uint8_t)preamble,
        HEADER_EXPLICIT, payload_len, CRC_ON, IQ_NORMAL
    };
    return cmd_write(CMD_SET_PACKET_PARAMS, p, sizeof(p));
}

static bool map_irq(uint16_t irq_mask, uint16_t dio1_mask)
{
    uint8_t p[8] = {
        (uint8_t)(irq_mask >> 8),  (uint8_t)irq_mask,
        (uint8_t)(dio1_mask >> 8), (uint8_t)dio1_mask,
        0U, 0U, 0U, 0U
    };
    return cmd_write(CMD_SET_DIO_IRQ_PARAMS, p, sizeof(p));
}

static bool set_rf_frequency(uint32_t hz)
{
    uint32_t frf = (uint32_t)(((uint64_t)hz << 25) / 32000000ULL);
    uint8_t p[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                    (uint8_t)(frf >> 8), (uint8_t)frf};
    return cmd_write(CMD_SET_RF_FREQUENCY, p, sizeof(p));
}

static bool calibrate_image(uint32_t hz)
{
    uint8_t p[2];
    uint32_t mhz = hz / 1000000UL;
    if (mhz >= 900U)      { p[0] = 0xE1U; p[1] = 0xE9U; }
    else if (mhz >= 850U) { p[0] = 0xD7U; p[1] = 0xDBU; }
    else if (mhz >= 779U) { p[0] = 0xC1U; p[1] = 0xC5U; }
    else if (mhz >= 470U) { p[0] = 0x75U; p[1] = 0x81U; }
    else                  { p[0] = 0x6BU; p[1] = 0x6FU; }
    return cmd_write(CMD_CALIBRATE_IMAGE, p, sizeof(p));
}

bool nrfclaw_llcc68_rl_wakeup(void)
{
    if (!bus_acquire()) return false;
    if (!m_radio_sleeping) return wait_busy();

    /* LLCC68 wake from SLEEP is initiated by NSS falling edge.  Keep SCK idle
     * during the documented wake interval before asking BUSY for readiness. */
    cs_low();
    nrf_delay_us(200);
    cs_high();
    if (!wait_busy()) return false;
    m_radio_sleeping = false;
    return true;
}

bool nrfclaw_llcc68_rl_standby(void)
{
    uint8_t p = STANDBY_RC;
    return cmd_write(CMD_SET_STANDBY, &p, 1U);
}

bool nrfclaw_llcc68_rl_sleep(void)
{
    if (m_radio_sleeping) { bus_release(); return true; }
    uint8_t p = 0x04U; /* warm start, retain active LoRa configuration */
    if (!cmd_write(CMD_SET_SLEEP, &p, 1U)) return false;
    /* Datasheet requires about 500 us with no further SPI while retention is saved. */
    nrf_delay_us(600);
    m_radio_sleeping = true;
    bus_release();
    return true;
}

bool nrfclaw_llcc68_rl_clear_irq(uint16_t mask)
{
    uint8_t p[2] = {(uint8_t)(mask >> 8), (uint8_t)mask};
    return cmd_write(CMD_CLEAR_IRQ_STATUS, p, sizeof(p));
}

bool nrfclaw_llcc68_rl_get_irq(uint16_t *irq)
{
    uint8_t tx[4] = {CMD_GET_IRQ_STATUS, 0U, 0U, 0U};
    uint8_t rx[4] = {0};
    if (!irq || !wait_busy()) return false;
    cs_low();
    bool ok = xfer(tx, rx, sizeof(tx));
    cs_high();
    if (!ok || !wait_busy()) return false;
    *irq = (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
    return true;
}

bool nrfclaw_llcc68_rl_apply_profile(nrfclaw_lora_profile_t const *profile)
{
    if (!profile || !nrfclaw_lora_profile_validate(profile)) return false;
    if (!nrfclaw_llcc68_rl_wakeup()) return false;
    if (!nrfclaw_llcc68_rl_standby()) return false;
    if (!calibrate_image(profile->frequency_hz)) return false;
    if (!set_rf_frequency(profile->frequency_hz)) return false;

    uint8_t pa[4] = {0x04U, 0x07U, 0x00U, 0x01U};
    if (!cmd_write(CMD_SET_PA_CONFIG, pa, sizeof(pa))) return false;
    uint8_t txp[2] = {(uint8_t)profile->power_dbm, RAMP_200_US};
    if (!cmd_write(CMD_SET_TX_PARAMS, txp, sizeof(txp))) return false;

    uint8_t mod[4] = {profile->sf, bw_code(profile->bw_khz), profile->cr, ldro(profile)};
    if (!cmd_write(CMD_SET_MODULATION_PARAMS, mod, sizeof(mod))) return false;

    uint8_t base[2] = {0U, 0U};
    if (!cmd_write(CMD_SET_BUFFER_BASE_ADDRESS, base, sizeof(base))) return false;

    /* Same nibble mapping used by SX126x/RadioLib for an 8-bit LoRa sync word. */
    uint8_t sync[2] = {
        (uint8_t)((profile->sync_word & 0xF0U) | 0x04U),
        (uint8_t)(((profile->sync_word & 0x0FU) << 4) | 0x04U)
    };
    if (!write_reg(REG_LORA_SYNC_MSB, sync, sizeof(sync))) return false;

    m_profile = *profile;
    return nrfclaw_llcc68_rl_clear_irq(0xFFFFU);
}

bool nrfclaw_llcc68_rl_init(nrfclaw_lora_profile_t const *profile)
{
    m_radio_sleeping = false;
    nrf_gpio_cfg_output(P_LORA_RXEN); nrf_gpio_pin_clear(P_LORA_RXEN);
    nrf_gpio_cfg_output(P_LORA_TXEN); nrf_gpio_pin_clear(P_LORA_TXEN);
    if (!bus_acquire()) return false;

    /* RadioLib-style reset robustness: reset, then insist on a working standby. */
    nrf_gpio_pin_clear(P_LORA_NRST); nrf_delay_ms(1);
    nrf_gpio_pin_set(P_LORA_NRST);   nrf_delay_ms(5);
    bool standby_ok = false;
    for (uint8_t i = 0U; i < 20U && !standby_ok; ++i) {
        standby_ok = nrfclaw_llcc68_rl_standby();
        if (!standby_ok) nrf_delay_ms(5);
    }
    if (!standby_ok) return false;

    uint8_t regulator = REGULATOR_DCDC;
    if (!cmd_write(CMD_SET_REGULATOR_MODE, &regulator, 1U)) return false;
    uint8_t type = PACKET_TYPE_LORA;
    if (!cmd_write(CMD_SET_PACKET_TYPE, &type, 1U)) return false;

    if (!nrfclaw_llcc68_rl_apply_profile(profile)) return false;
    return nrfclaw_llcc68_rl_sleep();
}

bool nrfclaw_llcc68_rl_start_tx(uint8_t const *data, uint8_t len)
{
    if (!data || len == 0U || len > 64U) return false;
    if (!nrfclaw_llcc68_rl_wakeup() || !nrfclaw_llcc68_rl_standby()) return false;
    if (!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) return false;
    if (!set_packet_params(len)) return false;
    if (!map_irq(NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT,
                 NRFCLAW_LLCC68_IRQ_TX_DONE)) return false;

    uint8_t p[65] = {0};
    p[0] = 0U;
    memcpy(&p[1], data, len);
    if (!cmd_write(CMD_WRITE_BUFFER, p, (uint8_t)(len + 1U))) return false;

    uint32_t t = RX_TIMEOUT_4S;
    uint8_t timeout[3] = {(uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t};
    return cmd_write(CMD_SET_TX, timeout, sizeof(timeout));
}

bool nrfclaw_llcc68_rl_finish_tx(void)
{
    bool ok = nrfclaw_llcc68_rl_clear_irq(0xFFFFU);
    ok = nrfclaw_llcc68_rl_standby() && ok;
    ok = nrfclaw_llcc68_rl_sleep() && ok;
    return ok;
}

static bool prepare_rx(void)
{
    if (!nrfclaw_llcc68_rl_standby()) return false;
    if (!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) return false;
    /* Explicit header uses the received header length; 0xFF is max payload. */
    if (!set_packet_params(0xFFU)) return false;
    uint16_t mask = NRFCLAW_LLCC68_IRQ_RX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT |
                    NRFCLAW_LLCC68_IRQ_CRC_ERR | NRFCLAW_LLCC68_IRQ_HEADER_ERR;
    return map_irq(mask, mask);
}

bool nrfclaw_llcc68_rl_start_rx(bool infinite)
{
    if (!nrfclaw_llcc68_rl_wakeup()) return false;
    if (!prepare_rx()) return false;
    uint32_t t = infinite ? RX_CONTINUOUS : RX_TIMEOUT_4S;
    uint8_t timeout[3] = {(uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t};
    return cmd_write(CMD_SET_RX, timeout, sizeof(timeout));
}

bool nrfclaw_llcc68_rl_start_rx_ms(uint32_t timeout_ms)
{
    uint32_t ticks;
    uint8_t timeout[3];

    if (timeout_ms == 0U || timeout_ms > 262143UL)
        return false;

    if (!nrfclaw_llcc68_rl_wakeup())
        return false;
    if (!prepare_rx())
        return false;

    /* SX126x/LLCC68 SetRx timeout unit is 15.625 us = 64 ticks/ms. */
    ticks = timeout_ms * 64UL;
    if (ticks > 0xFFFFFFUL)
        ticks = 0xFFFFFFUL;

    timeout[0] = (uint8_t)(ticks >> 16);
    timeout[1] = (uint8_t)(ticks >> 8);
    timeout[2] = (uint8_t)ticks;
    return cmd_write(CMD_SET_RX, timeout, sizeof(timeout));
}

bool nrfclaw_llcc68_rl_rearm_rx(void)
{
    /* Important R3.8.17 behavior: do not rely on implicit continuous-RX state
     * after RX_DONE. Re-stage RX immediately after consuming each IRQ. */
    if (!prepare_rx()) return false;
    uint8_t timeout[3] = {0xFFU, 0xFFU, 0xFFU};
    return cmd_write(CMD_SET_RX, timeout, sizeof(timeout));
}

bool nrfclaw_llcc68_rl_finish_rx(void)
{
    bool ok = nrfclaw_llcc68_rl_standby();
    ok = nrfclaw_llcc68_rl_clear_irq(0xFFFFU) && ok;
    ok = nrfclaw_llcc68_rl_sleep() && ok;
    return ok;
}

bool nrfclaw_llcc68_rl_read_packet(uint8_t *data, uint8_t *len, uint8_t max_len,
                                    int16_t *rssi_dbm_x2, int16_t *snr_db_x4)
{
    if (!data || !len || !rssi_dbm_x2 || !snr_db_x4) return false;

    uint8_t txs[4] = {CMD_GET_RX_BUFFER_STATUS, 0U, 0U, 0U};
    uint8_t rxs[4] = {0};
    if (!wait_busy()) return false;
    cs_low(); bool ok = xfer(txs, rxs, sizeof(txs)); cs_high();
    if (!ok || !wait_busy()) return false;
    uint8_t n = rxs[2];
    uint8_t offset = rxs[3];
    if (n == 0U || n > max_len) return false;

    uint8_t tx[67] = {0};
    uint8_t rx[67] = {0};
    tx[0] = CMD_READ_BUFFER; tx[1] = offset; tx[2] = 0U;
    if (!wait_busy()) return false;
    cs_low(); ok = xfer(tx, rx, (size_t)n + 3U); cs_high();
    if (!ok || !wait_busy()) return false;
    memcpy(data, &rx[3], n);
    *len = n;

    uint8_t tps[5] = {CMD_GET_PACKET_STATUS, 0U, 0U, 0U, 0U};
    uint8_t rps[5] = {0};
    if (!wait_busy()) return false;
    cs_low(); ok = xfer(tps, rps, sizeof(tps)); cs_high();
    if (!ok || !wait_busy()) return false;
    *rssi_dbm_x2 = -(int16_t)rps[2];
    *snr_db_x4 = (int16_t)(int8_t)rps[3];
    return true;
}
