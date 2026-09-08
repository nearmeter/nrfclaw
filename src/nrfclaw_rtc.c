#include "nrfclaw_rtc.h"

#include "nrf.h"
#include "nrfclaw_event.h"

/*
 * nRF52832 RTC PRESCALER is 12 bits:
 *
 *     fRTC = 32768 / (PRESCALER + 1)
 *
 * Maximum prescaler = 4095, therefore the slowest native RTC rate is:
 *
 *     32768 / 4096 = 8 Hz
 *
 * Stage 6 exposes time in whole seconds/Unix epoch, but internally RTC2 runs
 * at 8 ticks per second.
 */
#define NRFCLAW_RTC_PRESCALER          4095UL
#define NRFCLAW_RTC_TICKS_PER_SECOND   8UL
#define NRFCLAW_RTC_COUNTER_MASK       0x00FFFFFFUL
#define NRFCLAW_RTC_COUNTER_MODULO     0x01000000ULL

/*
 * A single RTC compare must occur before the 24-bit counter returns to the
 * same value. Keep a small safety margin.
 */
#define NRFCLAW_RTC_MAX_ALARM_TICKS    0x00FFFF00UL
#define NRFCLAW_RTC_MAX_ALARM_SECONDS  \
    (NRFCLAW_RTC_MAX_ALARM_TICKS / NRFCLAW_RTC_TICKS_PER_SECOND)


static volatile uint32_t m_wrap_count;

static volatile uint32_t m_epoch_at_sync;
static volatile uint64_t m_ticks_at_sync;
static volatile bool     m_epoch_valid;


/*
 * --------------------------------------------------------------------------
 * Extended RTC tick counter
 * --------------------------------------------------------------------------
 *
 * RTC2 itself is 24 bits. m_wrap_count extends it to 64 bits.
 *
 * IRQs are disabled only for a few instructions so COUNTER and wrap_count
 * form one consistent snapshot.
 */
static uint64_t rtc_ticks_now(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    uint32_t wraps =
        m_wrap_count;

    uint32_t counter =
        NRF_RTC2->COUNTER &
        NRFCLAW_RTC_COUNTER_MASK;

    /*
     * An overflow may already be pending but RTC2_IRQHandler may not have
     * executed yet. Account for that pending wrap in this snapshot.
     */
    if (NRF_RTC2->EVENTS_OVRFLW)
    {
        wraps++;

        /*
         * If COUNTER is still near the end of the previous cycle, the
         * overflow event belongs to a transition not yet reflected in the
         * sampled counter. Normally after overflow COUNTER is small.
         */
        if (counter > 0x00800000UL)
        {
            wraps--;
        }
    }

    if (!primask)
    {
        __enable_irq();
    }

    return
        ((uint64_t)wraps << 24) |
        (uint64_t)counter;
}


/*
 * --------------------------------------------------------------------------
 * RTC2 IRQ
 * --------------------------------------------------------------------------
 */
void RTC2_IRQHandler(void)
{
    if (NRF_RTC2->EVENTS_OVRFLW)
    {
        NRF_RTC2->EVENTS_OVRFLW = 0;

        m_wrap_count++;
    }


    if (NRF_RTC2->EVENTS_COMPARE[0])
    {
        NRF_RTC2->EVENTS_COMPARE[0] = 0;

        /*
         * One-shot alarm.
         */
        NRF_RTC2->INTENCLR =
            RTC_INTENCLR_COMPARE0_Msk;

        nrfclaw_event_t e =
        {
            .type = NRFCLAW_EVT_RTC
        };

        (void)nrfclaw_event_push_isr(
            &e
        );
    }
}


/*
 * --------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------
 */
void nrfclaw_rtc_init(void)
{
    NRF_RTC2->TASKS_STOP = 1;
    NRF_RTC2->TASKS_CLEAR = 1;

    NRF_RTC2->PRESCALER =
        NRFCLAW_RTC_PRESCALER;


    NRF_RTC2->EVENTS_OVRFLW = 0;
    NRF_RTC2->EVENTS_COMPARE[0] = 0;


    /*
     * Overflow event is needed to extend the 24-bit counter.
     *
     * COMPARE0 interrupt is enabled only when an alarm is armed.
     */
    NRF_RTC2->EVTENSET =
        RTC_EVTENSET_OVRFLW_Msk |
        RTC_EVTENSET_COMPARE0_Msk;

    NRF_RTC2->INTENSET =
        RTC_INTENSET_OVRFLW_Msk;


    m_wrap_count = 0U;

    m_epoch_at_sync = 0U;
    m_ticks_at_sync = 0ULL;
    m_epoch_valid = false;


    NVIC_SetPriority(
        RTC2_IRQn,
        7
    );

    NVIC_ClearPendingIRQ(
        RTC2_IRQn
    );

    NVIC_EnableIRQ(
        RTC2_IRQn
    );


    NRF_RTC2->TASKS_START = 1;
}


/*
 * --------------------------------------------------------------------------
 * Wall-clock synchronization
 * --------------------------------------------------------------------------
 */
void nrfclaw_rtc_set_epoch(uint32_t epoch_utc)
{
    m_ticks_at_sync =
        rtc_ticks_now();

    m_epoch_at_sync =
        epoch_utc;

    m_epoch_valid =
        true;
}


/*
 * --------------------------------------------------------------------------
 * Current Unix time
 * --------------------------------------------------------------------------
 */
uint32_t nrfclaw_rtc_now(void)
{
    uint64_t ticks =
        rtc_ticks_now();


    if (!m_epoch_valid)
    {
        /*
         * Before synchronization expose uptime seconds.
         */
        return
            (uint32_t)(
                ticks /
                NRFCLAW_RTC_TICKS_PER_SECOND
            );
    }


    uint64_t elapsed_ticks =
        ticks - m_ticks_at_sync;

    uint32_t elapsed_seconds =
        (uint32_t)(
            elapsed_ticks /
            NRFCLAW_RTC_TICKS_PER_SECOND
        );


    return
        m_epoch_at_sync +
        elapsed_seconds;
}


/*
 * --------------------------------------------------------------------------
 * One-shot alarm in Unix epoch
 * --------------------------------------------------------------------------
 */
bool nrfclaw_rtc_set_alarm_epoch(
    uint32_t epoch_utc
)
{
    uint32_t now =
        nrfclaw_rtc_now();


    uint32_t delta_seconds;

    if (epoch_utc <= now)
    {
        /*
         * Preserve previous Stage behavior: a past alarm is converted into
         * the next possible one-second wakeup instead of blocking forever.
         */
        delta_seconds = 1U;
    }
    else
    {
        delta_seconds =
            epoch_utc - now;
    }


    /*
     * One hardware compare cannot safely span a complete 24-bit RTC cycle.
     *
     * At 8 Hz this gives approximately 24.27 days, more than enough for
     * WEEKLY and normal EVERY rules.
     *
     * Longer AT schedules can later be implemented by chaining intermediate
     * compare events.
     */
    if (delta_seconds >=
        NRFCLAW_RTC_MAX_ALARM_SECONDS)
    {
        return false;
    }


    uint32_t delta_ticks =
        delta_seconds *
        NRFCLAW_RTC_TICKS_PER_SECOND;


    if (delta_ticks == 0U)
    {
        delta_ticks = 1U;
    }


    uint32_t current_counter =
        NRF_RTC2->COUNTER &
        NRFCLAW_RTC_COUNTER_MASK;


    uint32_t cc =
        (current_counter + delta_ticks) &
        NRFCLAW_RTC_COUNTER_MASK;


    /*
     * Avoid stale compare state.
     */
    NRF_RTC2->INTENCLR =
        RTC_INTENCLR_COMPARE0_Msk;

    NRF_RTC2->EVENTS_COMPARE[0] =
        0;


    NRF_RTC2->CC[0] =
        cc;


    NRF_RTC2->INTENSET =
        RTC_INTENSET_COMPARE0_Msk;


    return true;
}


/*
 * --------------------------------------------------------------------------
 * Alarm cancellation
 * --------------------------------------------------------------------------
 */
void nrfclaw_rtc_cancel_alarm(void)
{
    NRF_RTC2->INTENCLR =
        RTC_INTENCLR_COMPARE0_Msk;

    NRF_RTC2->EVENTS_COMPARE[0] =
        0;
}


bool nrfclaw_rtc_is_valid(void)
{
    return
        m_epoch_valid;
}
