#include "nrfclaw_battery.h"
#include "nrfclaw_board.h"

#include "app_error.h"
#include "app_timer.h"
#include "nrf_drv_saadc.h"

/*
 * Battery ADC input is board-specific. On nRF52832 only P0.02, P0.03,
 * P0.04, P0.05, P0.28, P0.29, P0.30 and P0.31 are SAADC inputs.
 * Translate board_config.h P_BAT to the matching AIN selector at compile time.
 */
#if P_BAT == 2
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN0
#elif P_BAT == 3
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN1
#elif P_BAT == 4
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN2
#elif P_BAT == 5
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN3
#elif P_BAT == 28
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN4
#elif P_BAT == 29
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN5
#elif P_BAT == 30
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN6
#elif P_BAT == 31
#define NRFCLAW_BATTERY_SAADC_INPUT NRF_SAADC_INPUT_AIN7
#else
#error "P_BAT must be an analog-capable nRF52832 pin (2,3,4,5,28,29,30,31)"
#endif

APP_TIMER_DEF(m_battery_timer);

typedef enum {
    BAT_IDLE = 0,
    BAT_SETTLE,
    BAT_DISCARD,
    BAT_SAMPLE
} battery_state_t;

static battery_state_t m_state;
static bool m_saadc_on;
static uint8_t m_count;
static uint32_t m_sum;
static uint16_t m_last_cv;
static bool m_last_valid;

static void timer_handler(void *ctx)
{
    (void)ctx;
    nrfclaw_event_t evt = { .type = NRFCLAW_EVT_BATTERY_STEP };
    (void)nrfclaw_event_push_isr(&evt);
}

static void schedule_ms(uint32_t ms)
{
    ret_code_t err = app_timer_start(m_battery_timer, APP_TIMER_TICKS(ms), NULL);
    APP_ERROR_CHECK(err);
}

static void saadc_start(void)
{
    if (m_saadc_on)
        return;

    nrf_drv_saadc_config_t cfg = NRF_DRV_SAADC_DEFAULT_CONFIG;

    /* The validated NINASENSE battery conversion uses a 10-bit ADC scale
     * (0..1023). Do not inherit the project-wide SAADC resolution here,
     * because sdk_config.h may be configured for 12 bits (0..4095), which
     * would make the calculated battery voltage approximately 4x too high. */
    cfg.resolution = NRF_SAADC_RESOLUTION_10BIT;

    APP_ERROR_CHECK(nrf_drv_saadc_init(&cfg, NULL));

    nrf_saadc_channel_config_t ch =
        NRF_DRV_SAADC_DEFAULT_CHANNEL_CONFIG_SE(NRFCLAW_BATTERY_SAADC_INPUT);

    ch.gain = NRF_SAADC_GAIN1_6;
    ch.reference = NRF_SAADC_REFERENCE_INTERNAL;

    APP_ERROR_CHECK(nrf_drv_saadc_channel_init(0, &ch));
    m_saadc_on = true;
}

static void saadc_stop(void)
{
    if (!m_saadc_on)
        return;

    /*
     * Stop any pending SAADC acquisition before uninitializing the driver.
     * This is intentionally a very short hardware synchronization wait,
     * not an application delay.
     */
    NRF_SAADC->EVENTS_STOPPED = 0;
    NRF_SAADC->TASKS_STOP = 1;

    while (NRF_SAADC->EVENTS_STOPPED == 0)
    {
        __NOP();
    }

    NRF_SAADC->EVENTS_STOPPED = 0;

    /*
     * Release the SDK driver/IRQ state.
     */
    nrf_drv_saadc_uninit();

    /*
     * Make the low-power state explicit.  The VM must only receive
     * BATTERY_DONE after the ADC hardware has been fully released.
     */
    NRF_SAADC->ENABLE = 0;

    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_NC;
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;

    m_saadc_on = false;
}

static uint16_t sample_once(void)
{
    nrf_saadc_value_t v = 0;
    APP_ERROR_CHECK(nrf_drv_saadc_sample_convert(0, &v));
    return (v < 0) ? 0U : (uint16_t)v;
}

static uint16_t raw_to_centivolts(uint16_t adc_avg)
{
    uint32_t vadc_mv;
    uint32_t vbat_mv;

    /* Preserve the conversion from the battery routine validated on NINASENSE.
     * 10-bit equivalent scale, 3.6 V full scale and divider factor 1.402. */
    vadc_mv = ((uint32_t)adc_avg * 3600UL + 511UL) / 1023UL;
    vbat_mv = (vadc_mv * 14020UL + 5000UL) / 10000UL;

    /* centivolts: 365 means 3.65 V */
    return (uint16_t)((vbat_mv + 5UL) / 10UL);
}

void nrfclaw_battery_init(void)
{
    m_state = BAT_IDLE;
    m_saadc_on = false;
    m_count = 0;
    m_sum = 0;
    m_last_cv = 0;
    m_last_valid = false;
    APP_ERROR_CHECK(app_timer_create(&m_battery_timer,
                                     APP_TIMER_MODE_SINGLE_SHOT,
                                     timer_handler));
}

bool nrfclaw_battery_last(uint16_t *centivolts)
{
    if (!centivolts || !m_last_valid) return false;
    *centivolts = m_last_cv;
    return true;
}

bool nrfclaw_battery_busy(void)
{
    return m_state != BAT_IDLE;
}

bool nrfclaw_battery_start(void)
{
    if (m_state != BAT_IDLE)
        return false;

    m_sum = 0;
    m_count = 0;
    saadc_start();

    /* The old routine used a 30 ms warm-up. Keep the validated timing but put
     * the CPU to sleep during the interval. */
    m_state = BAT_SETTLE;
    schedule_ms(30);
    return true;
}

void nrfclaw_battery_on_event(nrfclaw_event_t const *evt)
{
    if (!evt || evt->type != NRFCLAW_EVT_BATTERY_STEP)
        return;

    switch (m_state)
    {
        case BAT_SETTLE:
            m_count = 0;
            m_state = BAT_DISCARD;
            schedule_ms(10);
            break;

        case BAT_DISCARD:
            (void)sample_once();
            m_count++;

            if (m_count < 5U)
            {
                schedule_ms(10);
            }
            else
            {
                m_count = 0;
                m_sum = 0;
                m_state = BAT_SAMPLE;
                schedule_ms(10);
            }
            break;

        case BAT_SAMPLE:
            m_sum += sample_once();
            m_count++;

            if (m_count < 8U)
            {
                schedule_ms(10);
            }
            else
            {
                uint16_t avg = (uint16_t)(m_sum / 8U);
                uint16_t cv = raw_to_centivolts(avg);
                m_last_cv = cv;
                m_last_valid = true;
                nrfclaw_event_t done = {
                    .type = NRFCLAW_EVT_BATTERY_DONE,
                    .arg0 = cv,
                    .arg1 = avg
                };

                saadc_stop();
                m_state = BAT_IDLE;
                (void)nrfclaw_event_push(&done);
            }
            break;

        case BAT_IDLE:
        default:
            break;
    }
}
