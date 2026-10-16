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

#define PACKET_TYPE_GFSK            0x00U
#define PACKET_TYPE_LORA            0x01U
/* B8.0f GFSK RAW RX */
#define REG_GFSK_SYNC_WORD_0        0x06C0U

#define GFSK_FILTER_NONE            0x00U
#define GFSK_PREAMBLE_DETECT_OFF    0x00U
#define GFSK_PREAMBLE_DETECT_8      0x04U
#define GFSK_PREAMBLE_DETECT_16     0x05U
#define GFSK_PREAMBLE_DETECT_24     0x06U
#define GFSK_PREAMBLE_DETECT_32     0x07U
#define GFSK_ADDRESS_FILT_OFF       0x00U
#define GFSK_PACKET_VARIABLE         0x01U
#define GFSK_PACKET_FIXED           0x00U
#define GFSK_CRC_OFF                0x01U
#define GFSK_WHITENING_OFF          0x00U
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
/* B8.0f GFSK RAW RX */
static bool m_gfsk_active = false;
static uint8_t m_gfsk_payload_len = 21U;
static uint8_t m_gfsk_sync_len = 3U;
static uint16_t m_gfsk_preamble_bits = 24U;

static uint8_t m_gfsk_packet_mode = NRFCLAW_LLCC68_GFSK_PACKET_FIXED;
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
    if (m_gfsk_active) return false;
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
    if (m_gfsk_active) return false;
    if (!nrfclaw_llcc68_rl_wakeup()) return false;
    if (!prepare_rx()) return false;
    uint32_t t = infinite ? RX_CONTINUOUS : RX_TIMEOUT_4S;
    uint8_t timeout[3] = {(uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t};
    return cmd_write(CMD_SET_RX, timeout, sizeof(timeout));
}

bool nrfclaw_llcc68_rl_start_rx_ms(uint32_t timeout_ms)
{
    uint32_t ticks;
    if (m_gfsk_active) return false;
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
    if (m_gfsk_active) return false;
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

/* =========================================================================
 * B8.0f GFSK RAW RX
 * VEVOR YT60234: 915 MHz, 2-FSK, 11.26 kbaud, +/-37 kHz,
 * preamble AA AA AA, sync CA CA 54, fixed payload 21 bytes.
 * ========================================================================= */

static bool gfsk_bw_code_valid(uint8_t code)
{
    switch(code) {
    case 0x1FU: case 0x17U: case 0x0FU: case 0x1EU:
    case 0x16U: case 0x0EU: case 0x1DU: case 0x15U:
    case 0x0DU: case 0x1CU: case 0x14U: case 0x0CU:
    case 0x1BU: case 0x13U: case 0x0BU: case 0x1AU:
    case 0x12U: case 0x0AU: case 0x19U: case 0x11U:
    case 0x09U:
        return true;
    default:
        return false;
    }
}

static bool gfsk_frequency_sane(uint32_t frequency_hz)
{
    /*
     * Generic LLCC68 chip-level sanity only.
     * Populated-module capability (400/900 family) belongs to the
     * profile/module gate, not to this generic radio ABI.
     */
    return frequency_hz >= 150000000UL && frequency_hz <= 960000000UL;
}

static bool gfsk_set_modulation(uint32_t bitrate_bps,uint32_t deviation_hz,uint8_t rx_bw_code)
{
    if (bitrate_bps == 0U) return false;
    uint32_t br=(uint32_t)((1024000000ULL+(bitrate_bps/2U))/bitrate_bps);
    uint32_t fd=(uint32_t)(((uint64_t)deviation_hz<<25)/32000000ULL);
    if (br>0xFFFFFFUL || fd>0xFFFFFFUL) return false;
    uint8_t p[8]={(uint8_t)(br>>16),(uint8_t)(br>>8),(uint8_t)br,
                  GFSK_FILTER_NONE,rx_bw_code,
                  (uint8_t)(fd>>16),(uint8_t)(fd>>8),(uint8_t)fd};
    return cmd_write(CMD_SET_MODULATION_PARAMS,p,sizeof(p));
}

static uint8_t gfsk_preamble_detector(uint16_t preamble_bits,
                                      uint8_t sync_len)
{
    uint16_t limit = preamble_bits;
    uint16_t sync_bits = (uint16_t)sync_len * 8U;

    /*
     * SX126x GFSK: preamble detector length is limited by both the
     * configured preamble and the sync-word length.
     */
    if (sync_bits < limit)
        limit = sync_bits;

    if (limit >= 32U) return GFSK_PREAMBLE_DETECT_32;
    if (limit >= 24U) return GFSK_PREAMBLE_DETECT_24;
    if (limit >= 16U) return GFSK_PREAMBLE_DETECT_16;
    if (limit >=  8U) return GFSK_PREAMBLE_DETECT_8;
    return GFSK_PREAMBLE_DETECT_OFF;
}


static bool gfsk_set_packet_mode(uint8_t payload_len,
                                 uint16_t preamble_bits,
                                 uint8_t sync_len,
                                 uint8_t packet_mode)
{
    uint8_t p[9];

    if(payload_len==0U || sync_len==0U || sync_len>4U ||
       preamble_bits<8U ||
       packet_mode>NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE)
        return false;

    p[0]=(uint8_t)(preamble_bits>>8);
    p[1]=(uint8_t)preamble_bits;
    p[2]=GFSK_PREAMBLE_DETECT_24;
    p[3]=(uint8_t)(sync_len*8U);
    p[4]=GFSK_ADDRESS_FILT_OFF;
    p[5]=(packet_mode==NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE) ?
         GFSK_PACKET_VARIABLE : GFSK_PACKET_FIXED;
    p[6]=payload_len;
    p[7]=GFSK_CRC_OFF;
    p[8]=GFSK_WHITENING_OFF;
    return cmd_write(CMD_SET_PACKET_PARAMS,p,sizeof(p));
}

static bool gfsk_set_packet(uint8_t payload_len,
                            uint16_t preamble_bits,
                            uint8_t sync_len)
{
    return gfsk_set_packet_mode(
        payload_len,preamble_bits,sync_len,
        NRFCLAW_LLCC68_GFSK_PACKET_FIXED);
}

static bool gfsk_set_rx_continuous(void)
{
    uint8_t t[3]={0xFFU,0xFFU,0xFFU};
    return cmd_write(CMD_SET_RX,t,sizeof(t));
}
static bool gfsk_read_fifo_chunks(uint8_t offset,
                                  uint8_t *data,
                                  size_t len)
{
    while(len!=0U){
        uint8_t chunk=(uint8_t)(len>64U?64U:len);
        uint8_t tx[67]={0};
        uint8_t rx[67]={0};
        bool ok;

        tx[0]=CMD_READ_BUFFER;
        tx[1]=offset;
        tx[2]=0U;

        if(!wait_busy())
            return false;
        cs_low();
        ok=xfer(tx,rx,(size_t)chunk+3U);
        cs_high();
        if(!ok || !wait_busy())
            return false;

        memcpy(data,&rx[3],chunk);
        data+=chunk;
        offset=(uint8_t)(offset+chunk);
        len-=chunk;
    }
    return true;
}

static bool gfsk_read_buffer(uint8_t *data,
                             size_t *len,
                             size_t max_len)
{
    uint8_t txs[4]={CMD_GET_RX_BUFFER_STATUS,0U,0U,0U};
    uint8_t rxs[4]={0};
    uint8_t n;
    uint8_t off;
    size_t total;
    bool variable;

    if(!data || !len || !wait_busy())
        return false;

    cs_low();
    {
        bool ok=xfer(txs,rxs,sizeof(txs));
        cs_high();
        if(!ok || !wait_busy())
            return false;
    }

    n=rxs[2];
    off=rxs[3];
    variable=(m_gfsk_packet_mode==
              NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE);

    total=(size_t)n+(variable?1U:0U);
    if(total==0U || total>max_len)
        return false;

    if(variable){
        /*
         * SX126x/LLCC68 variable-length mode consumes the one-byte
         * packet length field in hardware. Wireless M-Bus Format B
         * uses exactly that byte as L, so reconstruct it before RFVM.
         */
        data[0]=n;
        if(n!=0U && !gfsk_read_fifo_chunks(off,&data[1],n))
            return false;
    }else{
        if(n==0U || !gfsk_read_fifo_chunks(off,data,n))
            return false;
    }

    *len=total;
    return true;
}

/* B8.1k6a3 — RX-only Generic FSK PHY envelope.
 *
 * Receive-side support for E220-900MM22S / LLCC68 protocols outside
 * the 902..928 MHz ISM-915 window.
 *
 * TX policy is intentionally unchanged.
 */
bool nrfclaw_llcc68_rl_gfsk_start_profile(
    nrfclaw_llcc68_gfsk_profile_t const *cfg)
{
    if(!cfg) return false;
    if(cfg->packet_mode>NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE)
        return false;
    if(!gfsk_frequency_sane(cfg->frequency_hz)) return false;
    if(cfg->bitrate_bps==0U || cfg->deviation_hz==0U ||
       cfg->payload_len==0U || cfg->sync_len==0U || cfg->sync_len>4U)
        return false;
    if(!gfsk_bw_code_valid(cfg->rx_bw_code)) return false;



    if(m_gfsk_active){
        if(!nrfclaw_llcc68_rl_standby()) return false;
        if(!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) return false;
        m_gfsk_active=false;
    }

    if(!nrfclaw_llcc68_rl_wakeup()) goto fail;
    if(!nrfclaw_llcc68_rl_standby()) goto fail;
    { uint8_t type=PACKET_TYPE_GFSK;
      if(!cmd_write(CMD_SET_PACKET_TYPE,&type,1U)) goto fail; }
    if(!calibrate_image(cfg->frequency_hz)) goto fail;
    if(!set_rf_frequency(cfg->frequency_hz)) goto fail;
    if(!gfsk_set_modulation(cfg->bitrate_bps,cfg->deviation_hz,
                            cfg->rx_bw_code)) goto fail;
    { uint8_t base[2]={0U,0U};
      if(!cmd_write(CMD_SET_BUFFER_BASE_ADDRESS,base,sizeof(base))) goto fail; }
    if(!write_reg(REG_GFSK_SYNC_WORD_0,cfg->sync,cfg->sync_len)) goto fail;
    if(cfg->packet_mode==NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE){
        if(!gfsk_set_packet_mode(
                cfg->payload_len,cfg->preamble_bits,cfg->sync_len,
                NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE))
            goto fail;
    }else{
        if(!gfsk_set_packet(
                cfg->payload_len,cfg->preamble_bits,cfg->sync_len))
            goto fail;
    }
    if(!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) goto fail;
    { uint16_t mask=NRFCLAW_LLCC68_IRQ_RX_DONE|NRFCLAW_LLCC68_IRQ_TIMEOUT;
      if(!map_irq(mask,mask)) goto fail; }

    nrf_gpio_cfg_output(P_LORA_RXEN);
    nrf_gpio_cfg_output(P_LORA_TXEN);
    nrf_gpio_pin_set(P_LORA_RXEN);
    nrf_gpio_pin_clear(P_LORA_TXEN);

    if(!gfsk_set_rx_continuous()) goto fail;
    m_gfsk_payload_len=cfg->payload_len;
    m_gfsk_sync_len=cfg->sync_len;
    m_gfsk_preamble_bits=cfg->preamble_bits;
    m_gfsk_packet_mode=cfg->packet_mode;
    m_gfsk_active=true;
    return true;

fail:
    m_gfsk_active=false;
    nrf_gpio_pin_clear(P_LORA_RXEN);
    nrf_gpio_pin_clear(P_LORA_TXEN);
    { uint8_t type=PACKET_TYPE_LORA;
      (void)cmd_write(CMD_SET_PACKET_TYPE,&type,1U); }
    (void)nrfclaw_llcc68_rl_apply_profile(&m_profile);
    (void)nrfclaw_llcc68_rl_sleep();
    return false;
}

bool nrfclaw_llcc68_rl_gfsk_start_vevor(uint32_t frequency_hz,
                                         uint8_t rx_bw_code)
{
    nrfclaw_llcc68_gfsk_profile_t cfg;
    memset(&cfg,0,sizeof(cfg));
    cfg.frequency_hz=frequency_hz;
    cfg.bitrate_bps=11260UL;
    cfg.deviation_hz=37000UL;
    cfg.rx_bw_code=rx_bw_code;
    cfg.preamble_bits=24U;
    cfg.sync[0]=0xCAU; cfg.sync[1]=0xCAU; cfg.sync[2]=0x54U;
    cfg.sync_len=3U;
    cfg.payload_len=21U;
    return nrfclaw_llcc68_rl_gfsk_start_profile(&cfg);
}
bool nrfclaw_llcc68_rl_gfsk_poll_ex(
    uint16_t *irq,
    uint8_t *data,
    size_t *len,
    size_t max_len,
    bool *packet_ready)
{
    if(!m_gfsk_active || !irq || !len || !packet_ready)
        return false;

    *packet_ready=false;
    *len=0U;

    if(!nrfclaw_llcc68_rl_get_irq(irq))
        return false;

    if((*irq&NRFCLAW_LLCC68_IRQ_RX_DONE)!=0U){
        if(!data || !gfsk_read_buffer(data,len,max_len))
            return false;

        *packet_ready=true;

        if(!nrfclaw_llcc68_rl_clear_irq(0xFFFFU))
            return false;
        if(!nrfclaw_llcc68_rl_standby())
            return false;

        if(m_gfsk_packet_mode==NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE){
            if(!gfsk_set_packet_mode(
                    m_gfsk_payload_len,
                    m_gfsk_preamble_bits,
                    m_gfsk_sync_len,
                    NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE))
                return false;
        }else{
            /* Preserve the frozen B8.1g fixed-profile re-arm contract. */
            if(!gfsk_set_packet(m_gfsk_payload_len,m_gfsk_preamble_bits,m_gfsk_sync_len))
                return false;
        }

        if(!gfsk_set_rx_continuous())
            return false;
    }

    return true;
}

bool nrfclaw_llcc68_rl_gfsk_poll(
    uint16_t *irq,
    uint8_t *data,
    uint8_t *len,
    uint8_t max_len,
    bool *packet_ready)
{
    size_t n=0U;
    bool ready=false;

    if(!len || !packet_ready)
        return false;

    if(!nrfclaw_llcc68_rl_gfsk_poll_ex(
            irq,data,&n,max_len,&ready))
        return false;

    /*
     * Legacy diagnostics are one-byte-length ABI. Full 256-byte
     * variable frames are consumed only by automatic ingest via poll_ex.
     */
    if(n>255U)
        return false;

    *len=(uint8_t)n;
    *packet_ready=ready;
    return true;
}


bool nrfclaw_llcc68_rl_gfsk_stop_restore_lora(void)
{
    bool ok=true;
    if(!m_gfsk_active) return true;
    ok=nrfclaw_llcc68_rl_standby()&&ok;
    ok=nrfclaw_llcc68_rl_clear_irq(0xFFFFU)&&ok;
    nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_clear(P_LORA_TXEN);
    { uint8_t type=PACKET_TYPE_LORA; ok=cmd_write(CMD_SET_PACKET_TYPE,&type,1U)&&ok; }
    m_gfsk_active=false;
    ok=nrfclaw_llcc68_rl_apply_profile(&m_profile)&&ok;
    ok=nrfclaw_llcc68_rl_sleep()&&ok;
    return ok;
}

/* =========================================================================
 * B8.0g SYNTHETIC VEVOR TX
 * One-shot bench transmitter for the documented YT60234 reference frame.
 * The LLCC68 packet engine generates the 24-bit preamble and CA CA 54 sync.
 * Only the 21-byte payload is written to FIFO.
 * ========================================================================= */
bool nrfclaw_llcc68_rl_gfsk_tx_profile(
    nrfclaw_llcc68_gfsk_profile_t const *cfg,
    uint8_t const *payload,
    uint8_t payload_len,
    int8_t power_dbm,
    uint16_t *terminal_irq)
{
    bool ok=false;
    uint16_t irq=0U;
    uint8_t wb[65];

    if(!cfg || !payload || !terminal_irq) return false;
    if(cfg->packet_mode!=NRFCLAW_LLCC68_GFSK_PACKET_FIXED)
        return false;
    *terminal_irq=0U;
    if(payload_len==0U || payload_len>64U || payload_len!=cfg->payload_len) return false;
    if(!gfsk_frequency_sane(cfg->frequency_hz)) return false;
    if(cfg->bitrate_bps==0U || cfg->deviation_hz==0U || cfg->sync_len==0U || cfg->sync_len>4U) return false;
    if(!gfsk_bw_code_valid(cfg->rx_bw_code)) return false;
    if(power_dbm < -9 || power_dbm > 14) return false;
    if(m_gfsk_active) return false;

    if(!nrfclaw_llcc68_rl_wakeup()) goto restore;
    if(!nrfclaw_llcc68_rl_standby()) goto restore;
    { uint8_t type=PACKET_TYPE_GFSK; if(!cmd_write(CMD_SET_PACKET_TYPE,&type,1U)) goto restore; }
    if(!calibrate_image(cfg->frequency_hz)) goto restore;
    if(!set_rf_frequency(cfg->frequency_hz)) goto restore;
    if(!gfsk_set_modulation(cfg->bitrate_bps,cfg->deviation_hz,cfg->rx_bw_code)) goto restore;
    { uint8_t base[2]={0U,0U}; if(!cmd_write(CMD_SET_BUFFER_BASE_ADDRESS,base,sizeof(base))) goto restore; }
    if(!write_reg(REG_GFSK_SYNC_WORD_0,cfg->sync,cfg->sync_len)) goto restore;
    if(!gfsk_set_packet(payload_len,cfg->preamble_bits,cfg->sync_len)) goto restore;
    {
        uint8_t pa[4]={0x04U,0x07U,0x00U,0x01U};
        uint8_t txp[2]={(uint8_t)power_dbm,RAMP_200_US};
        if(!cmd_write(CMD_SET_PA_CONFIG,pa,sizeof(pa))) goto restore;
        if(!cmd_write(CMD_SET_TX_PARAMS,txp,sizeof(txp))) goto restore;
    }
    if(!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) goto restore;
    if(!map_irq(NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT, NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT)) goto restore;
    memset(wb,0,sizeof(wb)); wb[0]=0U; memcpy(&wb[1],payload,payload_len);
    if(!cmd_write(CMD_WRITE_BUFFER,wb,(size_t)(payload_len+1U))) goto restore;
    nrf_gpio_cfg_output(P_LORA_RXEN); nrf_gpio_cfg_output(P_LORA_TXEN);
    nrf_gpio_pin_clear(P_LORA_RXEN); nrf_gpio_pin_set(P_LORA_TXEN);
    {
        uint32_t ticks=1000UL*64UL;
        uint8_t t[3]={(uint8_t)(ticks>>16),(uint8_t)(ticks>>8),(uint8_t)ticks};
        if(!cmd_write(CMD_SET_TX,t,sizeof(t))) goto restore;
    }
    for(uint16_t i=0U;i<250U;++i){
        if(!nrfclaw_llcc68_rl_get_irq(&irq)) goto restore;
        if((irq & (NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT)) != 0U) break;
        nrf_delay_ms(1);
    }
    *terminal_irq=irq;
    ok=((irq & NRFCLAW_LLCC68_IRQ_TX_DONE)!=0U) && ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT)==0U);
restore:
    nrf_gpio_pin_clear(P_LORA_TXEN); nrf_gpio_pin_clear(P_LORA_RXEN);
    (void)nrfclaw_llcc68_rl_standby(); (void)nrfclaw_llcc68_rl_clear_irq(0xFFFFU);
    { uint8_t type=PACKET_TYPE_LORA; (void)cmd_write(CMD_SET_PACKET_TYPE,&type,1U); }
    (void)nrfclaw_llcc68_rl_apply_profile(&m_profile);
    (void)nrfclaw_llcc68_rl_sleep();
    return ok;
}

bool nrfclaw_llcc68_rl_gfsk_tx_vevor_variant(uint32_t frequency_hz,
                                              int8_t power_dbm,
                                              uint8_t variant,
                                              uint16_t *terminal_irq)
{
    static const uint8_t sync[3] = {0xCAU,0xCAU,0x54U};
    uint8_t payload[21] = {
        0xAAU,0x00U,0xF8U,0xF7U,0x9DU,0x02U,0xE3U,
        0x32U,0x01U,0x0EU,0x03U,0x02U,0x0BU,0x01U,
        0x38U,0x02U,0x39U,0x7AU,0x86U,0xE0U,0x87U
    };
    if (variant > 1U) return false;
    if (variant == 1U) {
        /* B8.0k1 LIVE HA PUSH VECTOR
         * Same sensor id/channel; changed measurements only.
         * temp=28.7C hum=63 wind=21 gust=8 dir=310 rain=77 uv=4. */
        payload[5]=0x03U; payload[6]=0x13U;
        payload[7]=0x3FU;
        payload[8]=0x01U; payload[9]=0x16U;
        payload[10]=0x08U;
        payload[11]=0x02U; payload[12]=0x37U;
        payload[13]=0x01U; payload[14]=0x4EU;
        payload[15]=0x05U;
        { uint8_t sum=0U; for(uint8_t i=0U;i<19U;++i) sum=(uint8_t)(sum+payload[i]); payload[19]=sum; }
    }
    bool ok = false;
    uint16_t irq = 0U;

    if (!terminal_irq) return false;
    *terminal_irq = 0U;
    if (frequency_hz < 902000000UL || frequency_hz > 928000000UL)
        return false;
    if (power_dbm < -9 || power_dbm > 14)
        return false;

    /* TX is one-shot. Do not steal a receiver already owned by B8.0f. */
    if (m_gfsk_active)
        return false;

    if (!nrfclaw_llcc68_rl_wakeup()) goto restore;
    if (!nrfclaw_llcc68_rl_standby()) goto restore;

    {
        uint8_t type = PACKET_TYPE_GFSK;
        if (!cmd_write(CMD_SET_PACKET_TYPE,&type,1U)) goto restore;
    }

    if (!calibrate_image(frequency_hz)) goto restore;
    if (!set_rf_frequency(frequency_hz)) goto restore;
    if (!gfsk_set_modulation(11260UL,37000UL,
                             NRFCLAW_LLCC68_GFSK_BW_117_3)) goto restore;

    {
        uint8_t base[2] = {0U,0U};
        if (!cmd_write(CMD_SET_BUFFER_BASE_ADDRESS,base,sizeof(base)))
            goto restore;
    }

    if (!write_reg(REG_GFSK_SYNC_WORD_0,sync,sizeof(sync))) goto restore;
    if (!gfsk_set_packet((uint8_t)sizeof(payload),24U,3U)) goto restore;

    /* Reuse the PA configuration already proven by the normal LoRa backend,
     * but keep the bench default at 0 dBm to avoid near-field overload. */
    {
        uint8_t pa[4] = {0x04U,0x07U,0x00U,0x01U};
        uint8_t txp[2] = {(uint8_t)power_dbm,RAMP_200_US};
        if (!cmd_write(CMD_SET_PA_CONFIG,pa,sizeof(pa))) goto restore;
        if (!cmd_write(CMD_SET_TX_PARAMS,txp,sizeof(txp))) goto restore;
    }

    if (!nrfclaw_llcc68_rl_clear_irq(0xFFFFU)) goto restore;
    if (!map_irq(NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT,
                 NRFCLAW_LLCC68_IRQ_TX_DONE | NRFCLAW_LLCC68_IRQ_TIMEOUT))
        goto restore;

    {
        uint8_t wb[22] = {0U};
        wb[0] = 0U;
        memcpy(&wb[1],payload,sizeof(payload));
        if (!cmd_write(CMD_WRITE_BUFFER,wb,sizeof(wb))) goto restore;
    }

    nrf_gpio_cfg_output(P_LORA_RXEN);
    nrf_gpio_cfg_output(P_LORA_TXEN);
    nrf_gpio_pin_clear(P_LORA_RXEN);
    nrf_gpio_pin_set(P_LORA_TXEN);

    {
        /* 1 second is far longer than the ~20 ms packet airtime. */
        uint32_t ticks = 1000UL * 64UL;
        uint8_t t[3] = {
            (uint8_t)(ticks >> 16),
            (uint8_t)(ticks >> 8),
            (uint8_t)ticks
        };
        if (!cmd_write(CMD_SET_TX,t,sizeof(t))) goto restore;
    }

    /* Synchronous lab gate: poll for at most ~250 ms. */
    for (uint16_t i = 0U; i < 250U; ++i) {
        if (!nrfclaw_llcc68_rl_get_irq(&irq)) goto restore;
        if ((irq & (NRFCLAW_LLCC68_IRQ_TX_DONE |
                    NRFCLAW_LLCC68_IRQ_TIMEOUT)) != 0U)
            break;
        nrf_delay_ms(1);
    }

    *terminal_irq = irq;
    ok = ((irq & NRFCLAW_LLCC68_IRQ_TX_DONE) != 0U) &&
         ((irq & NRFCLAW_LLCC68_IRQ_TIMEOUT) == 0U);

restore:
    nrf_gpio_pin_clear(P_LORA_TXEN);
    nrf_gpio_pin_clear(P_LORA_RXEN);
    (void)nrfclaw_llcc68_rl_standby();
    (void)nrfclaw_llcc68_rl_clear_irq(0xFFFFU);
    {
        uint8_t type = PACKET_TYPE_LORA;
        (void)cmd_write(CMD_SET_PACKET_TYPE,&type,1U);
    }
    (void)nrfclaw_llcc68_rl_apply_profile(&m_profile);
    (void)nrfclaw_llcc68_rl_sleep();
    return ok;
}

bool nrfclaw_llcc68_rl_gfsk_tx_vevor(uint32_t frequency_hz,
                                      int8_t power_dbm,
                                      uint16_t *terminal_irq)
{
    return nrfclaw_llcc68_rl_gfsk_tx_vevor_variant(
        frequency_hz,power_dbm,0U,terminal_irq);
}

bool nrfclaw_llcc68_rl_gfsk_active(void){ return m_gfsk_active; }


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
