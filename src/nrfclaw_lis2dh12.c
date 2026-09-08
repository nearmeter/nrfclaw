#include "nrfclaw_lis2dh12.h"
#include "app_timer.h"
#include "app_error.h"
#include "nrfclaw_board.h"
#include "nrfclaw_event.h"
#include "nrf.h"
#include "nrf_gpio.h"
#include "nrf_drv_gpiote.h"
#include "app_error.h"
#include "nrf_delay.h"
#include <string.h>

#define LIS_ADDR_LOW  0x18U
#define LIS_ADDR_HIGH 0x19U
#define LIS_WHO_AM_I 0x0FU
#define LIS_WHO_AM_I_VALUE 0x33U
#define LIS_CTRL1 0x20U
#define LIS_CTRL2 0x21U
#define LIS_CTRL3 0x22U
#define LIS_CTRL4 0x23U
#define LIS_CTRL5 0x24U
#define LIS_CTRL6 0x25U
#define LIS_REFERENCE 0x26U
#define LIS_STATUS 0x27U
#define LIS_OUT_X_L 0x28U
#define LIS_FIFO_CTRL 0x2EU
#define LIS_FIFO_SRC 0x2FU
#define LIS_INT1_CFG 0x30U
#define LIS_INT1_SRC 0x31U
#define LIS_INT1_THS 0x32U
#define LIS_INT1_DURATION 0x33U
#define LIS_CLICK_CFG 0x38U
#define LIS_CLICK_SRC 0x39U
#define LIS_CLICK_THS 0x3AU
#define LIS_TIME_LIMIT 0x3BU
#define LIS_TIME_LATENCY 0x3CU
#define LIS_TIME_WINDOW 0x3DU

static bool m_present, m_irq_configured;
static uint8_t m_i2c_addr = LIS_ADDR_LOW;
static uint8_t m_last_whoami = 0x00U;
static nrfclaw_accel_mode_t m_mode;
static nrfclaw_accel_config_t m_cfg;
static nrfclaw_lis2dh12_xyz_t m_vib[NRFCLAW_ACCEL_VIB_MAX_SAMPLES];
static nrfclaw_accel_vibration_metrics_t m_vib_metrics;
static bool m_vib_valid;
static uint32_t m_vib_seq;

/* r3.8.11: driver-level FIFO watchdog.  INT2 remains preferred; this
 * one-shot fallback guarantees the 200 Hz/high-resolution sensor cannot stay
 * powered indefinitely if the external watermark interrupt is absent. */
#define VIB_WATCHDOG_FIRST_MS 200U
#define VIB_WATCHDOG_RETRY_MS 100U
#define VIB_WATCHDOG_MAX_POLLS 3U
APP_TIMER_DEF(m_vib_watchdog_timer);
static bool m_vib_watchdog_due;
static bool m_vib_watchdog_armed;
static bool m_vib_watchdog_failed;
static uint8_t m_vib_watchdog_polls;
static uint16_t m_vib_watchdog_elapsed_ms;
static uint8_t m_walk_phase;
static nrfclaw_lis2dh12_diag_t m_diag;

static void diag_inc(uint16_t *v){if(v&&*v<65535U)(*v)++;}

static bool wait_event_or_error(volatile uint32_t *evt)
{
    uint32_t t=120000UL;
    while((*evt==0U) && (NRF_TWI1->EVENTS_ERROR==0U) && t--) {}
    return (*evt!=0U) && (NRF_TWI1->EVENTS_ERROR==0U);
}

static bool wait_stopped(void)
{
    uint32_t t=120000UL;
    while((NRF_TWI1->EVENTS_STOPPED==0U) && t--) {}
    return NRF_TWI1->EVENTS_STOPPED!=0U;
}

static void twi_hw_disable(void)
{
    NRF_TWI1->SHORTS=0U;
    NRF_TWI1->ENABLE=TWI_ENABLE_ENABLE_Disabled;
}

/*
 * r3.8.9: every TWI transaction must leave the peripheral completely
 * detached from the bus. Merely writing ENABLE=Disabled leaves PSELSCL/
 * PSELSDA associated with TWI1. That is harmless most of the time, but after
 * an error/recovery or a long FIFO burst it can leave the serial block/pins
 * in a higher-current residual state.
 *
 * Keep SCL/SDA as pulled-up inputs while idle. This is board-independent and
 * safe both with and without external I2C pull-ups; it also prevents floating
 * LIS2DH12 inputs. twi_enable() reconnects PSEL on the next transaction.
 */
static void twi_low_power_release(void)
{
    twi_hw_disable();
    NRF_TWI1->PSELSCL = 0xFFFFFFFFUL;
    NRF_TWI1->PSELSDA = 0xFFFFFFFFUL;
    nrf_gpio_cfg_input(P_LIS_SCL, NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(P_LIS_SDA, NRF_GPIO_PIN_PULLUP);
    diag_inc(&m_diag.twi_low_power_releases);

    /* Snapshot the post-cleanup state without performing any bus access. */
    m_diag.twi_last_psel_scl_disconnected = (NRF_TWI1->PSELSCL == 0xFFFFFFFFUL) ? 1U : 0U;
    m_diag.twi_last_psel_sda_disconnected = (NRF_TWI1->PSELSDA == 0xFFFFFFFFUL) ? 1U : 0U;
    m_diag.twi_last_scl_pin_cnf = NRF_P0->PIN_CNF[P_LIS_SCL];
    m_diag.twi_last_sda_pin_cnf = NRF_P0->PIN_CNF[P_LIS_SDA];
    m_diag.twi_last_errorsrc = NRF_TWI1->ERRORSRC;
    m_diag.twi_last_hfclk_running = ((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk) != 0U) ? 1U : 0U;
}

static void twi_clear_state(void)
{
    NRF_TWI1->SHORTS=0U;
    NRF_TWI1->EVENTS_STOPPED=0U;
    NRF_TWI1->EVENTS_ERROR=0U;
    NRF_TWI1->EVENTS_TXDSENT=0U;
    NRF_TWI1->EVENTS_RXDREADY=0U;
    NRF_TWI1->ERRORSRC=NRF_TWI1->ERRORSRC;
}

static void twi_bus_recover(void)
{
    /*
     * A slave may keep SDA low if a previous transaction was aborted before
     * STOP completed. Release SDA and clock SCL up to 9 times, then synthesize
     * a STOP (SDA low -> SCL high -> SDA high).
     *
     * S0D1 is appropriate for an I2C-style open-drain line: drive low for 0,
     * disconnect for 1, with the pull-up providing the HIGH level.
     */
    twi_hw_disable();

    nrf_gpio_cfg(P_LIS_SCL,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_PULLUP,
                 NRF_GPIO_PIN_S0D1,
                 NRF_GPIO_PIN_NOSENSE);
    nrf_gpio_cfg(P_LIS_SDA,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_PULLUP,
                 NRF_GPIO_PIN_S0D1,
                 NRF_GPIO_PIN_NOSENSE);

    nrf_gpio_pin_set(P_LIS_SDA);
    nrf_gpio_pin_set(P_LIS_SCL);
    nrf_delay_us(5U);

    for(uint8_t i=0U;i<9U && nrf_gpio_pin_read(P_LIS_SDA)==0U;i++){
        nrf_gpio_pin_clear(P_LIS_SCL);
        nrf_delay_us(5U);
        nrf_gpio_pin_set(P_LIS_SCL);
        nrf_delay_us(5U);
    }

    /* STOP: SDA low while SCL high, then release SDA. */
    nrf_gpio_pin_clear(P_LIS_SDA);
    nrf_delay_us(5U);
    nrf_gpio_pin_set(P_LIS_SCL);
    nrf_delay_us(5U);
    nrf_gpio_pin_set(P_LIS_SDA);
    nrf_delay_us(5U);

    nrf_gpio_cfg_input(P_LIS_SDA,NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(P_LIS_SCL,NRF_GPIO_PIN_PULLUP);
}

static void twi_enable(void)
{
    /* CS HIGH selects I2C mode. Keep the bus at 100 kHz. */
    nrf_gpio_cfg_input(P_LIS_SDA,NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(P_LIS_SCL,NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_output(P_LIS_CS);
    nrf_gpio_pin_set(P_LIS_CS);
    nrf_delay_us(10U);

    NRF_TWI1->ENABLE=TWI_ENABLE_ENABLE_Disabled;
    NRF_TWI1->PSELSCL=P_LIS_SCL;
    NRF_TWI1->PSELSDA=P_LIS_SDA;
    NRF_TWI1->FREQUENCY=TWI_FREQUENCY_FREQUENCY_K100;
    NRF_TWI1->ADDRESS=m_i2c_addr;
    NRF_TWI1->ENABLE=TWI_ENABLE_ENABLE_Enabled;
    twi_clear_state();
}

static bool twi_abort_and_recover(void)
{
    /*
     * Ask the peripheral to generate STOP and, critically, wait for it before
     * disabling TWI. If STOP itself cannot complete, recover with GPIO clocks.
     */
    NRF_TWI1->SHORTS=0U;
    NRF_TWI1->EVENTS_STOPPED=0U;
    NRF_TWI1->TASKS_STOP=1U;
    bool stopped=wait_stopped();
    twi_hw_disable();
    twi_clear_state();
    if(!stopped) twi_bus_recover();
    twi_low_power_release();
    return false;
}

static void twi_finish_after_stop(void)
{
    /*
     * STOP has already been generated (explicitly or by BB_STOP). Never
     * disable TWI until EVENTS_STOPPED proves that SDA/SCL were released.
     */
    (void)wait_stopped();
    twi_hw_disable();
    twi_clear_state();
    twi_low_power_release();
}

static void release_pins(void)
{
    NRF_TWI1->SHORTS=0U;
    NRF_TWI1->EVENTS_STOPPED=0U;
    NRF_TWI1->TASKS_STOP=1U;
    (void)wait_stopped();
    twi_hw_disable();
    NRF_TWI1->PSELSCL = 0xFFFFFFFFUL;
    NRF_TWI1->PSELSDA = 0xFFFFFFFFUL;
    nrf_gpio_cfg_default(P_LIS_SCL);
    nrf_gpio_cfg_default(P_LIS_CS);
    nrf_gpio_cfg_default(P_LIS_SDA);
    nrf_gpio_cfg_default(P_LIS_INT1);
    nrf_gpio_cfg_default(P_LIS_INT2);
}

static bool write_bytes(const uint8_t*d,uint8_t len)
{
    if(!d||!len) return false;

    twi_enable();
    NRF_TWI1->TASKS_STARTTX=1U;

    for(uint8_t i=0U;i<len;i++){
        NRF_TWI1->EVENTS_TXDSENT=0U;
        NRF_TWI1->TXD=d[i];
        if(!wait_event_or_error(&NRF_TWI1->EVENTS_TXDSENT))
            return twi_abort_and_recover();
    }

    NRF_TWI1->EVENTS_STOPPED=0U;
    NRF_TWI1->TASKS_STOP=1U;
    if(!wait_stopped())
        return twi_abort_and_recover();

    twi_hw_disable();
    twi_clear_state();
    twi_low_power_release();
    return true;
}

static bool reg_write(uint8_t r,uint8_t v)
{
    uint8_t b[2]={r,v};
    bool ok=write_bytes(b,2U);
    if(!ok)diag_inc(&m_diag.reg_write_failures);
    return ok;
}

static bool reg_read_raw(uint8_t reg,uint8_t*d,uint8_t len)
{
    if(!d||!len) return false;

    twi_enable();

    /* Register-address write phase. */
    NRF_TWI1->TASKS_STARTTX=1U;
    NRF_TWI1->EVENTS_TXDSENT=0U;
    NRF_TWI1->TXD=reg;
    if(!wait_event_or_error(&NRF_TWI1->EVENTS_TXDSENT))
        return twi_abort_and_recover();

    /*
     * Repeated START read phase.
     * BB_STOP on the final byte generates the bus STOP automatically.
     * For intermediate bytes BB_SUSPEND lets firmware switch the shortcut
     * before RESUME.
     */
    NRF_TWI1->EVENTS_RXDREADY=0U;
    NRF_TWI1->EVENTS_STOPPED=0U;
    NRF_TWI1->SHORTS=(len==1U)?TWI_SHORTS_BB_STOP_Msk
                              :TWI_SHORTS_BB_SUSPEND_Msk;
    NRF_TWI1->TASKS_STARTRX=1U;

    for(uint8_t i=0U;i<len;i++){
        if(!wait_event_or_error(&NRF_TWI1->EVENTS_RXDREADY))
            return twi_abort_and_recover();

        d[i]=(uint8_t)NRF_TWI1->RXD;

        if(i+1U<len){
            NRF_TWI1->EVENTS_RXDREADY=0U;
            NRF_TWI1->SHORTS=(i+2U==len)?TWI_SHORTS_BB_STOP_Msk
                                        :TWI_SHORTS_BB_SUSPEND_Msk;
            NRF_TWI1->TASKS_RESUME=1U;
        }
    }

    /*
     * This wait is the essential r2g fix. In r2f the peripheral was disabled
     * immediately after the last RXDREADY, racing the hardware-generated STOP
     * and occasionally leaving the LIS2DH12/I2C bus wedged.
     */
    if(!wait_stopped())
        return twi_abort_and_recover();

    twi_hw_disable();
    twi_clear_state();
    twi_low_power_release();
    return true;
}

static bool reg_read(uint8_t reg,uint8_t*d,uint8_t len)
{
    bool ok=reg_read_raw(reg,d,len);
    if(!ok)diag_inc(&m_diag.reg_read_failures);
    return ok;
}
static void vib_watchdog_stop(void)
{
    (void)app_timer_stop(m_vib_watchdog_timer);
    m_vib_watchdog_due=false;
    m_vib_watchdog_armed=false;
}

static bool vib_watchdog_start_ms(uint16_t delay_ms)
{
    (void)app_timer_stop(m_vib_watchdog_timer);
    m_vib_watchdog_due=false;
    uint32_t rc=app_timer_start(m_vib_watchdog_timer,APP_TIMER_TICKS(delay_ms),NULL);
    if(rc!=NRF_SUCCESS){
        m_vib_watchdog_armed=false;
        m_vib_watchdog_failed=true;
        diag_inc(&m_diag.fifo_watchdog_aborts);
        m_diag.fifo_last_completion_source=3U;
        return false;
    }
    m_vib_watchdog_armed=true;
    return true;
}

static void vib_watchdog_timer_handler(void *ctx)
{
    (void)ctx;
    m_vib_watchdog_due=true;
    nrfclaw_event_t e={.type=NRFCLAW_EVT_ACCEL_VIBRATION_POLL,.arg0=1U,.arg1=0U};
    (void)nrfclaw_event_push_isr(&e);
}

static bool vib_watchdog_arm_initial(void)
{
    m_vib_watchdog_polls=0U;m_vib_watchdog_elapsed_ms=0U;m_vib_watchdog_failed=false;
    diag_inc(&m_diag.fifo_watchdog_arms);
    return vib_watchdog_start_ms(VIB_WATCHDOG_FIRST_MS);
}

static void lis_irq_handler(nrf_drv_gpiote_pin_t pin,nrf_gpiote_polarity_t a){(void)a;if(pin==P_LIS_INT1)nrfclaw_lis2dh12_on_int1_isr();else if(pin==P_LIS_INT2)nrfclaw_lis2dh12_on_int2_isr();}
static void irq_setup(void){if(m_irq_configured||!m_present)return;if(!nrf_drv_gpiote_is_init())APP_ERROR_CHECK(nrf_drv_gpiote_init());nrf_drv_gpiote_in_config_t c=GPIOTE_CONFIG_IN_SENSE_LOTOHI(false);c.pull=NRF_GPIO_PIN_NOPULL;APP_ERROR_CHECK(nrf_drv_gpiote_in_init(P_LIS_INT1,&c,lis_irq_handler));APP_ERROR_CHECK(nrf_drv_gpiote_in_init(P_LIS_INT2,&c,lis_irq_handler));nrf_drv_gpiote_in_event_enable(P_LIS_INT1,true);nrf_drv_gpiote_in_event_enable(P_LIS_INT2,true);m_irq_configured=true;}

static uint8_t odr_bits(uint16_t hz){switch(hz){case 1:return 0x10;case 10:return 0x20;case 25:return 0x30;case 50:return 0x40;case 100:return 0x50;case 200:return 0x60;case 400:return 0x70;default:return 0;}}
static uint8_t fs_bits(uint8_t g){switch(g){case 2:return 0x00;case 4:return 0x10;case 8:return 0x20;case 16:return 0x30;default:return 0xFF;}}
static uint16_t scale_mg_per_lsb(uint8_t g){return g==4?2U:g==8?4U:g==16?12U:1U;}
static uint16_t isqrt32(uint32_t x){uint32_t op=x,res=0,one=1UL<<30;while(one>op)one>>=2;while(one){if(op>=res+one){op-=res+one;res=(res>>1)+one;}else res>>=1;one>>=2;}return (uint16_t)(res>65535U?65535U:res);}

void nrfclaw_lis2dh12_init(void)
{
    nrfclaw_lis2dh12_diag_reset();
    APP_ERROR_CHECK(app_timer_create(&m_vib_watchdog_timer,APP_TIMER_MODE_SINGLE_SHOT,vib_watchdog_timer_handler));
    m_vib_watchdog_due=false;m_vib_watchdog_armed=false;m_vib_watchdog_failed=false;m_vib_watchdog_polls=0U;m_vib_watchdog_elapsed_ms=0U;
    uint8_t who=0U;
    m_present=false;
    m_irq_configured=false;
    m_mode=NRFCLAW_ACCEL_MODE_OFF;
    m_vib_valid=false;
    m_vib_seq=0U;
    m_walk_phase=0U;
    m_last_whoami=0U;

    /* LIS2DH12 powers up with CTRL_REG1 ODR=0000 (power-down), but the
     * digital interface and WHO_AM_I remain accessible.  No ODR write is
     * required before probing.  SA0 selects 0x18 or 0x19, so try both. */
    const uint8_t addresses[2]={LIS_ADDR_LOW,LIS_ADDR_HIGH};
    for(uint8_t i=0U;i<2U;i++){
        m_i2c_addr=addresses[i];
        who=0U;
        if(reg_read(LIS_WHO_AM_I,&who,1U)){
            m_last_whoami=who;
            if(who==LIS_WHO_AM_I_VALUE){m_present=true;break;}
        }
    }

    if(!m_present){release_pins();return;}

    /* Probe only: leave the sensing engine powered down until a profile is
     * requested.  This keeps the idle current at the minimum. */
    (void)reg_write(LIS_CTRL1,0x00);
    (void)reg_write(LIS_CTRL3,0U);
    (void)reg_write(LIS_CTRL5,0U);
    (void)reg_write(LIS_CTRL6,0U);
    irq_setup();
}
bool nrfclaw_lis2dh12_present(void){return m_present;}
uint8_t nrfclaw_lis2dh12_i2c_address(void){return m_i2c_addr;}
uint8_t nrfclaw_lis2dh12_last_whoami(void){return m_last_whoami;}
nrfclaw_accel_mode_t nrfclaw_lis2dh12_mode(void){return m_mode;}

static int16_t decode_sample(const uint8_t*l,uint8_t g,bool lp){int16_t raw=(int16_t)((uint16_t)l[0]|((uint16_t)l[1]<<8));int32_t v;if(lp){int16_t q=(int16_t)(raw>>8);uint16_t sens=(g==4)?32U:(g==8)?64U:(g==16)?192U:16U;v=(int32_t)q*sens;}else{v=(int32_t)(raw>>4)*(int32_t)scale_mg_per_lsb(g);}if(v>32767)v=32767;if(v<-32768)v=-32768;return (int16_t)v;}
bool nrfclaw_lis2dh12_read_xyz(nrfclaw_lis2dh12_xyz_t*o)
{
    if(!o||!m_present) return false;

    /*
     * Snapshot semantics for PUBLIC ACCEL_READ:
     *
     * After boot/probe the LIS2DH12 is intentionally left in power-down
     * (CTRL_REG1 ODR=0000). Reading OUT_X/Y/Z in that state returns the
     * last latched conversion forever. If no runtime profile is active,
     * temporarily wake the sensor at 100 Hz high-resolution, acquire a
     * guaranteed fresh XYZ sample, then immediately return to power-down.
     *
     * If MOTION/TAP/FALL/WALK/VIBRATION is already active, do not disturb
     * the profile: simply read the current sample using that profile's
     * scale/low-power decoding.
     */
    bool snapshot = (m_mode == NRFCLAW_ACCEL_MODE_OFF);
    uint8_t g = m_cfg.full_scale_g ? m_cfg.full_scale_g : 2U;
    bool lp = m_cfg.low_power;

    if(snapshot) {
        g = 2U;
        lp = false;

        /*
         * One-shot snapshot uses high-resolution at 100 Hz.
         *
         * LIS2DH12 high-resolution turn-on time is 7/ODR.  At 10 Hz that
         * would be ~700 ms, which is why the previous r2c/r2d snapshot
         * timed out after ~160 ms and NDP reported BUSY.  At 100 Hz the
         * specified turn-on time is ~70 ms, keeping the snapshot both fast
         * and low-energy while retaining the existing 12-bit/mg decoder.
         */
        if(!reg_write(LIS_CTRL4,0x88U)) return false; /* BDU=1, HR=1, +-2 g */
        if(!reg_write(LIS_CTRL1,0x57U)) return false; /* 100 Hz, XYZ enabled */

        /*
         * Do not make the snapshot depend on STATUS_REG.ZYXDA.
         *
         * On the target board WHO_AM_I and register writes are valid, but
         * requiring the ZYXDA latch caused the public ACCEL snapshot to
         * report BUSY indefinitely.  A deterministic acquisition is simpler
         * and more robust:
         *
         *  1. wait beyond the documented HR turn-on time (~70 ms @ 100 Hz)
         *  2. read/discard one complete XYZ sample
         *  3. wait one full ODR period
         *  4. read the next complete XYZ sample
         *
         * With BDU=1 the six output bytes remain coherent during each read.
         */
        nrf_delay_ms(85U);

        uint8_t discard[6];
        if(!reg_read((uint8_t)(LIS_OUT_X_L|0x80U),discard,6U)) {
            uint8_t dummy=0U;
            (void)reg_write(LIS_CTRL1,0x07U);
            (void)reg_read(LIS_REFERENCE,&dummy,1U);
            return false;
        }

        /* 100 Hz => 10 ms/sample. Use 12 ms margin for a definitely new one. */
        nrf_delay_ms(12U);
    }

    uint8_t raw[6];
    bool ok=reg_read((uint8_t)(LIS_OUT_X_L|0x80U),raw,6U);

    if(snapshot) {
        /*
         * Return to power-down after the snapshot.  ST recommends reading
         * REFERENCE after HR -> power-down to reset the filtering block
         * before the next normal/high-resolution activation.
         */
        uint8_t dummy=0U;
        (void)reg_write(LIS_CTRL1,0x07U);
        (void)reg_read(LIS_REFERENCE,&dummy,1U);
    }

    if(!ok) return false;

    o->x_mg=decode_sample(&raw[0],g,lp);
    o->y_mg=decode_sample(&raw[2],g,lp);
    o->z_mg=decode_sample(&raw[4],g,lp);
    return true;
}

void nrfclaw_lis2dh12_disable(void){vib_watchdog_stop();if(!m_present)return;(void)reg_write(LIS_CTRL1,0);(void)reg_write(LIS_CTRL3,0);(void)reg_write(LIS_CTRL5,0);(void)reg_write(LIS_CTRL6,0);(void)reg_write(LIS_FIFO_CTRL,0);(void)reg_write(LIS_INT1_CFG,0);(void)reg_write(LIS_CLICK_CFG,0);m_mode=NRFCLAW_ACCEL_MODE_OFF;m_vib_valid=false;}

bool nrfclaw_lis2dh12_configure(const nrfclaw_accel_config_t*c){if(!m_present||!c)return false;if(c->mode==NRFCLAW_ACCEL_MODE_OFF){nrfclaw_lis2dh12_disable();return true;}uint8_t odr=odr_bits(c->odr_hz),fs=fs_bits(c->full_scale_g);if(!odr||fs==0xFF)return false;m_cfg=*c;m_mode=c->mode;m_vib_valid=false;(void)reg_write(LIS_CTRL1,0);(void)reg_write(LIS_CTRL2,0);(void)reg_write(LIS_CTRL3,0);(void)reg_write(LIS_CTRL5,0);(void)reg_write(LIS_CTRL6,0);(void)reg_write(LIS_FIFO_CTRL,0);(void)reg_write(LIS_INT1_CFG,0);(void)reg_write(LIS_CLICK_CFG,0);
    /* HR for measurement quality; MOTION/WALK may select low-power through LPen. */
    uint8_t ctrl1=(uint8_t)(odr|0x07U|(c->low_power?0x08U:0));
    uint8_t ctrl4=(uint8_t)(fs|(c->low_power?0x00U:0x08U));
    (void)reg_write(LIS_CTRL4,ctrl4);(void)reg_write(LIS_CTRL1,ctrl1);
    uint16_t lsb=(uint16_t)(16U*c->full_scale_g/2U); if(lsb==0)lsb=16U;
    uint8_t th=(uint8_t)(c->threshold_mg/lsb);if(th==0)th=1;if(th>0x7F)th=0x7F;
    uint32_t ticks=((uint32_t)c->duration_ms*c->odr_hz+999U)/1000U;if(ticks>0x7F)ticks=0x7F;
    uint8_t dummy;
    switch(c->mode){
      case NRFCLAW_ACCEL_MODE_MOTION:
      case NRFCLAW_ACCEL_MODE_WALK:
        (void)reg_read(LIS_INT1_SRC,&dummy,1);(void)reg_write(LIS_INT1_THS,th);(void)reg_write(LIS_INT1_DURATION,(uint8_t)ticks);(void)reg_write(LIS_INT1_CFG,0x2A);(void)reg_write(LIS_CTRL3,0x40);break;
      case NRFCLAW_ACCEL_MODE_FALL:
        /* AND of X/Y/Z low events = free-fall primitive. */
        (void)reg_read(LIS_INT1_SRC,&dummy,1);(void)reg_write(LIS_INT1_THS,th);(void)reg_write(LIS_INT1_DURATION,(uint8_t)ticks);(void)reg_write(LIS_INT1_CFG,0x95);(void)reg_write(LIS_CTRL3,0x40);break;
      case NRFCLAW_ACCEL_MODE_TAP:
        (void)reg_read(LIS_CLICK_SRC,&dummy,1);(void)reg_write(LIS_CLICK_CFG,0x15);(void)reg_write(LIS_CLICK_THS,th&0x7F);(void)reg_write(LIS_TIME_LIMIT,0x08);(void)reg_write(LIS_TIME_LATENCY,0x10);(void)reg_write(LIS_TIME_WINDOW,0x20);(void)reg_write(LIS_CTRL3,0x80);break;
      case NRFCLAW_ACCEL_MODE_VIBRATION:
        if(c->odr_hz<100U)return false;/* FIFO stream, watermark at 32 samples */
        (void)reg_write(LIS_CTRL5,0x40);(void)reg_write(LIS_FIFO_CTRL,0x9E);/* watermark after 31 samples */(void)reg_write(LIS_CTRL6,0x04);/* WTM -> INT2 */
        if(!vib_watchdog_arm_initial()){nrfclaw_lis2dh12_disable();return false;}
        break;
      default:return false;
    }
    return true;
}

static bool diag_capture_motion_snapshot(void)
{
    /* Never read INT1_SRC here: that register is clear-on-read on the sensor
     * interrupt path.  The control/configuration registers below are safe. */
    uint8_t v[10];
    const uint8_t regs[10]={LIS_CTRL1,LIS_CTRL2,LIS_CTRL3,LIS_CTRL4,LIS_CTRL5,LIS_CTRL6,LIS_FIFO_CTRL,LIS_INT1_CFG,LIS_INT1_THS,LIS_INT1_DURATION};
    bool ok=true;
    for(uint8_t i=0U;i<10U;i++){
        if(!reg_read(regs[i],&v[i],1U)){ok=false;break;}
    }
    if(!ok){diag_inc(&m_diag.motion_snapshot_fail);return false;}
    diag_inc(&m_diag.motion_snapshot_count);
    m_diag.motion_ctrl1=v[0];m_diag.motion_ctrl2=v[1];m_diag.motion_ctrl3=v[2];m_diag.motion_ctrl4=v[3];
    m_diag.motion_ctrl5=v[4];m_diag.motion_ctrl6=v[5];m_diag.motion_fifo_ctrl=v[6];m_diag.motion_int1_cfg=v[7];
    m_diag.motion_int1_ths=v[8];m_diag.motion_int1_duration=v[9];
    return true;
}

bool nrfclaw_lis2dh12_configure_motion_hp(uint16_t threshold_mg,uint16_t duration_ms,uint16_t odr_hz){
    if(!m_present)return false;
    uint8_t odr=odr_bits(odr_hz);if(!odr||threshold_mg<16U)return false;
    nrfclaw_lis2dh12_disable();
    m_cfg.mode=NRFCLAW_ACCEL_MODE_MOTION;m_cfg.odr_hz=odr_hz;m_cfg.full_scale_g=2U;m_cfg.threshold_mg=threshold_mg;m_cfg.duration_ms=duration_ms;m_cfg.low_power=true;m_mode=NRFCLAW_ACCEL_MODE_MOTION;m_vib_valid=false;
    /* Low-power XYZ. HP_IA1 removes the DC/gravity component from IA1 while
     * keeping output registers unfiltered. HPM=00 is reset by REFERENCE. */
    if(!reg_write(LIS_CTRL4,0x00U))return false;
    if(!reg_write(LIS_CTRL1,(uint8_t)(odr|0x0FU)))return false;
    if(!reg_write(LIS_CTRL2,0x01U))return false; /* HP_IA1 */
    uint8_t dummy=0U;(void)reg_read(LIS_REFERENCE,&dummy,1U);(void)reg_read(LIS_INT1_SRC,&dummy,1U);
    uint8_t th=(uint8_t)(threshold_mg/16U);if(th==0U)th=1U;if(th>0x7FU)th=0x7FU;
    uint32_t ticks=((uint32_t)duration_ms*odr_hz+999U)/1000U;if(ticks>0x7FU)ticks=0x7FU;
    if(!reg_write(LIS_INT1_THS,th))return false;
    if(!reg_write(LIS_INT1_DURATION,(uint8_t)ticks))return false;
    if(!reg_write(LIS_INT1_CFG,0x2AU))return false; /* XH/YH/ZH OR */
    if(!reg_write(LIS_CTRL3,0x40U))return false;    /* IA1 -> INT1 */
    (void)diag_capture_motion_snapshot();
    return true;
}

bool nrfclaw_lis2dh12_enable_motion(uint16_t threshold_mg,uint8_t duration_ticks){
    /* r3.8.14b5: VM MOTION is a wake-on-movement primitive, not a gravity
     * threshold.  The generic MOTION configuration leaves IA1 unfiltered, so
     * the static ~1 g Z axis can immediately satisfy a 250 mg OR threshold and
     * generate a false MOTION event.  Use the already-validated HP IA1 path so
     * DC/gravity is removed before arming INT1. */
    uint16_t duration_ms=(uint16_t)duration_ticks*100U;
    if(duration_ms==0U)duration_ms=100U;
    return nrfclaw_lis2dh12_configure_motion_hp(threshold_mg,duration_ms,10U);
}
void nrfclaw_lis2dh12_disable_motion(void){nrfclaw_lis2dh12_disable();}

static void vibration_power_down_preserve_metrics(void)
{
    /* r3.8.11: a completed FIFO is data in RAM; there is no reason to leave
     * the LIS2DH12 running until the consumer eventually reads it.  Power the
     * sensor down immediately while preserving m_vib_valid/m_vib_metrics. */
    vib_watchdog_stop();
    if(!m_present)return;
    (void)reg_write(LIS_CTRL1,0);(void)reg_write(LIS_CTRL3,0);
    (void)reg_write(LIS_CTRL5,0);(void)reg_write(LIS_CTRL6,0);
    (void)reg_write(LIS_FIFO_CTRL,0);(void)reg_write(LIS_INT1_CFG,0);
    (void)reg_write(LIS_CLICK_CFG,0);
    m_mode=NRFCLAW_ACCEL_MODE_OFF;
}

static void vibration_capture(void){
    diag_inc(&m_diag.capture_attempts);
    uint8_t src=0;
    if(!reg_read(LIS_FIFO_SRC,&src,1))return;
    m_diag.last_fifo_src=src;
    uint8_t count=(uint8_t)(src&0x1FU);
    m_diag.last_fifo_count=count;
    if(count==0U){diag_inc(&m_diag.fifo_empty_polls);return;}
    if(count>31U)count=31U;

    int32_t sx=0,sy=0,sz=0;
    for(uint8_t i=0;i<count;i++){
        uint8_t raw[6];
        if(!reg_read((uint8_t)(LIS_OUT_X_L|0x80U),raw,6)){diag_inc(&m_diag.sample_read_failures);count=i;break;}
        m_vib[i].x_mg=decode_sample(&raw[0],m_cfg.full_scale_g,m_cfg.low_power);
        m_vib[i].y_mg=decode_sample(&raw[2],m_cfg.full_scale_g,m_cfg.low_power);
        m_vib[i].z_mg=decode_sample(&raw[4],m_cfg.full_scale_g,m_cfg.low_power);
        sx+=m_vib[i].x_mg;sy+=m_vib[i].y_mg;sz+=m_vib[i].z_mg;
    }
    if(!count)return;

    const int32_t mx=sx/count,my=sy/count,mz=sz/count;
    uint32_t sum2=0U,ex=0U,ey=0U,ez=0U;
    uint16_t peak=0U;
    int16_t minx=32767,maxx=-32768,miny=32767,maxy=-32768,minz=32767,maxz=-32768;

    /* r2d: RMS/peak are orientation-independent 3D quantities.  Accumulate
     * per-axis AC energy as well so P2P and zero-cross can use the dominant
     * vibration axis instead of being hard-wired to physical X. */
    for(uint8_t i=0;i<count;i++){
        int32_t dx=(int32_t)m_vib[i].x_mg-mx;
        int32_t dy=(int32_t)m_vib[i].y_mg-my;
        int32_t dz=(int32_t)m_vib[i].z_mg-mz;
        uint32_t x2=(uint32_t)(dx*dx),y2=(uint32_t)(dy*dy),z2=(uint32_t)(dz*dz);
        uint32_t e=x2+y2+z2;
        ex+=x2;ey+=y2;ez+=z2;sum2+=e/3U;
        uint16_t mag=isqrt32(e);if(mag>peak)peak=mag;
        if(m_vib[i].x_mg<minx)minx=m_vib[i].x_mg;if(m_vib[i].x_mg>maxx)maxx=m_vib[i].x_mg;
        if(m_vib[i].y_mg<miny)miny=m_vib[i].y_mg;if(m_vib[i].y_mg>maxy)maxy=m_vib[i].y_mg;
        if(m_vib[i].z_mg<minz)minz=m_vib[i].z_mg;if(m_vib[i].z_mg>maxz)maxz=m_vib[i].z_mg;
    }

    uint8_t dominant=0U;
    if(ey>ex&&ey>=ez)dominant=1U;else if(ez>ex&&ez>ey)dominant=2U;
    uint16_t p2p;
    if(dominant==1U)p2p=(uint16_t)((int32_t)maxy-(int32_t)miny);
    else if(dominant==2U)p2p=(uint16_t)((int32_t)maxz-(int32_t)minz);
    else p2p=(uint16_t)((int32_t)maxx-(int32_t)minx);

    uint16_t zc=0U;
    int32_t prev=(dominant==1U)?((int32_t)m_vib[0].y_mg-my):
                 (dominant==2U)?((int32_t)m_vib[0].z_mg-mz):((int32_t)m_vib[0].x_mg-mx);
    for(uint8_t i=1U;i<count;i++){
        int32_t cur=(dominant==1U)?((int32_t)m_vib[i].y_mg-my):
                    (dominant==2U)?((int32_t)m_vib[i].z_mg-mz):((int32_t)m_vib[i].x_mg-mx);
        if((prev<0&&cur>=0)||(prev>=0&&cur<0))zc++;
        prev=cur;
    }

    m_vib_metrics.sample_rate_hz=m_cfg.odr_hz;
    m_vib_metrics.sample_count=count;
    m_vib_metrics.rms_mg=isqrt32(sum2/count);
    m_vib_metrics.peak_mg=peak;
    m_vib_metrics.peak_to_peak_mg=p2p;
    m_vib_metrics.zero_cross_hz=(uint16_t)(((uint32_t)zc*m_cfg.odr_hz)/(2U*count));
    m_vib_metrics.sequence=++m_vib_seq;
    m_vib_valid=true;
    vibration_power_down_preserve_metrics();
    diag_inc(&m_diag.capture_success);
    nrfclaw_event_t e={.type=NRFCLAW_EVT_ACCEL_VIBRATION_READY,.arg0=m_vib_seq,.arg1=m_vib_metrics.rms_mg};
    (void)nrfclaw_event_push(&e);
}

bool nrfclaw_lis2dh12_vibration_service(void){
    diag_inc(&m_diag.vibration_service_calls);
    if(!m_present || m_mode!=NRFCLAW_ACCEL_MODE_VIBRATION) return false;
    if(m_vib_valid) return true;

    /*
     * r1e robustness path: INT2 remains the preferred autonomous wake source,
     * but an on-demand health request must not depend exclusively on the
     * external interrupt route. Read FIFO_SRC during the short CLI polling
     * window and capture through the same vibration_capture() path as INT2.
     */
    uint8_t src=0U;
    if(!reg_read(LIS_FIFO_SRC,&src,1U)) return false;
    m_diag.last_fifo_src=src;m_diag.last_fifo_count=(uint8_t)(src&0x1FU);
    /* r3.8.11: do not turn the watchdog fallback into a partial-window
     * sampler.  WTM=30 means 31 samples are expected before classification. */
    if((src & 0x1FU)<31U){diag_inc(&m_diag.fifo_empty_polls);return false;}

    vibration_capture();
    return m_vib_valid;
}

bool nrfclaw_lis2dh12_vibration_metrics(nrfclaw_accel_vibration_metrics_t*out){if(!out||!m_vib_valid)return false;*out=m_vib_metrics;return true;}
uint8_t nrfclaw_lis2dh12_vibration_count(void){return m_vib_valid?m_vib_metrics.sample_count:0;}
bool nrfclaw_lis2dh12_vibration_sample(uint8_t i,nrfclaw_lis2dh12_xyz_t*out){if(!out||!m_vib_valid||i>=m_vib_metrics.sample_count)return false;*out=m_vib[i];return true;}

void nrfclaw_lis2dh12_on_event(const nrfclaw_event_t*e)
{
    if(!e)return;
    if(e->type==NRFCLAW_EVT_ACCEL_INT2&&m_mode==NRFCLAW_ACCEL_MODE_VIBRATION){
        diag_inc(&m_diag.int2_event_captures);
        vibration_capture();
        if(m_vib_valid){
            vib_watchdog_stop();diag_inc(&m_diag.fifo_int2_completions);
            m_diag.fifo_last_completion_source=1U;
        }
        return;
    }
    if(e->type==NRFCLAW_EVT_ACCEL_VIBRATION_POLL&&m_mode==NRFCLAW_ACCEL_MODE_VIBRATION){
        if(!m_vib_watchdog_due)return;
        m_vib_watchdog_due=false;m_vib_watchdog_armed=false;
        diag_inc(&m_diag.fifo_watchdog_expirations);m_vib_watchdog_polls++;
        m_vib_watchdog_elapsed_ms=(uint16_t)(VIB_WATCHDOG_FIRST_MS + (uint16_t)(m_vib_watchdog_polls-1U)*VIB_WATCHDOG_RETRY_MS);
        if(nrfclaw_lis2dh12_vibration_service()){
            diag_inc(&m_diag.fifo_timer_completions);m_diag.fifo_last_completion_source=2U;
            m_diag.fifo_last_active_ms=m_vib_watchdog_elapsed_ms;vib_watchdog_stop();return;
        }
        if(m_vib_watchdog_polls<VIB_WATCHDOG_MAX_POLLS){
            diag_inc(&m_diag.fifo_watchdog_retries);
            if(vib_watchdog_start_ms(VIB_WATCHDOG_RETRY_MS))return;
        }
        /* Fail safe: never leave 200 Hz/high-resolution mode powered forever. */
        diag_inc(&m_diag.fifo_watchdog_aborts);m_diag.fifo_last_completion_source=3U;
        m_diag.fifo_last_active_ms=m_vib_watchdog_elapsed_ms;m_vib_watchdog_failed=true;
        nrfclaw_lis2dh12_disable();
    }
}
void nrfclaw_lis2dh12_on_int1_isr(void){diag_inc(&m_diag.int1_isr_count);nrfclaw_event_t e={0};if(m_mode==NRFCLAW_ACCEL_MODE_TAP)e.type=NRFCLAW_EVT_ACCEL_TAP;else if(m_mode==NRFCLAW_ACCEL_MODE_FALL)e.type=NRFCLAW_EVT_ACCEL_FALL;else if(m_mode==NRFCLAW_ACCEL_MODE_WALK){e.type=NRFCLAW_EVT_ACCEL_WALK;e.arg0=++m_walk_phase;}else e.type=NRFCLAW_EVT_ACCEL_MOTION;e.arg0=1;(void)nrfclaw_event_push_isr(&e);}
void nrfclaw_lis2dh12_on_int2_isr(void){diag_inc(&m_diag.int2_isr_count);nrfclaw_event_t e={.type=NRFCLAW_EVT_ACCEL_INT2,.arg0=1};(void)nrfclaw_event_push_isr(&e);}

bool nrfclaw_lis2dh12_vibration_watchdog_failed(void){return m_vib_watchdog_failed;}
void nrfclaw_lis2dh12_vibration_watchdog_clear_failed(void){m_vib_watchdog_failed=false;}

void nrfclaw_lis2dh12_diag_reset(void){memset(&m_diag,0,sizeof(m_diag));}
void nrfclaw_lis2dh12_diag_get(nrfclaw_lis2dh12_diag_t *out){if(out)*out=m_diag;}
void nrfclaw_lis2dh12_power_state_get(nrfclaw_lis2dh12_power_state_t *out)
{
    if(!out)return;
    memset(out,0,sizeof(*out));
    /* This function intentionally performs no I2C access and clears nothing. */
    out->twi_enabled=(NRF_TWI1->ENABLE==TWI_ENABLE_ENABLE_Enabled)?1U:0U;
    out->twi_psel_scl_disconnected=(NRF_TWI1->PSELSCL==0xFFFFFFFFUL)?1U:0U;
    out->twi_psel_sda_disconnected=(NRF_TWI1->PSELSDA==0xFFFFFFFFUL)?1U:0U;
    out->twi_errorsrc=(uint8_t)(NRF_TWI1->ERRORSRC & 0xFFU);
    out->hfclk_running=((NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_STATE_Msk)!=0U)?1U:0U;
    out->scl_pin_cnf=NRF_P0->PIN_CNF[P_LIS_SCL];
    out->sda_pin_cnf=NRF_P0->PIN_CNF[P_LIS_SDA];
    out->int1_level=(uint8_t)nrf_gpio_pin_read(P_LIS_INT1);
    out->int2_level=(uint8_t)nrf_gpio_pin_read(P_LIS_INT2);
    out->scl_level=(uint8_t)nrf_gpio_pin_read(P_LIS_SCL);
    out->sda_level=(uint8_t)nrf_gpio_pin_read(P_LIS_SDA);
    out->gpiote_events_port=(NRF_GPIOTE->EVENTS_PORT!=0U)?1U:0U;
    out->int1_latched=(NRF_P0->LATCH&(1UL<<P_LIS_INT1))?1U:0U;
    out->int2_latched=(NRF_P0->LATCH&(1UL<<P_LIS_INT2))?1U:0U;
    out->int1_sense=(uint8_t)((NRF_P0->PIN_CNF[P_LIS_INT1]&GPIO_PIN_CNF_SENSE_Msk)>>GPIO_PIN_CNF_SENSE_Pos);
    out->int2_sense=(uint8_t)((NRF_P0->PIN_CNF[P_LIS_INT2]&GPIO_PIN_CNF_SENSE_Msk)>>GPIO_PIN_CNF_SENSE_Pos);
    out->accel_mode=(uint8_t)m_mode;
}
