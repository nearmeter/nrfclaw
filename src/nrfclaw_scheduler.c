#include "nrfclaw_scheduler.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_vm.h"
#include "SEGGER_RTT.h"

static nrfclaw_schedule_t m_rule;
static nrfclaw_scheduler_state_t m_state;
static uint32_t m_next_epoch;
static bool m_manual_pause;
static bool m_programming_suspend;

static bool rule_valid(nrfclaw_schedule_t const *r)
{
    if (!r) return false;

    switch (r->mode)
    {
        case NRFCLAW_SCHEDULE_MANUAL:
        case NRFCLAW_SCHEDULE_BOOT:
            return true;

        case NRFCLAW_SCHEDULE_AT:
            return r->arg0 != 0U;

        case NRFCLAW_SCHEDULE_EVERY:
            return r->arg0 != 0U && r->arg0 < 0x00FFFF00UL;

        case NRFCLAW_SCHEDULE_WEEKLY:
            return (r->dow_mask & 0x7FU) != 0U && r->arg0 < 86400UL;

        default:
            return false;
    }
}

static uint32_t next_every(uint32_t now)
{
    uint32_t interval = m_rule.arg0;
    uint32_t anchor = m_rule.arg1 ? m_rule.arg1 : now;

    if (now < anchor)
        return anchor;

    uint32_t elapsed = now - anchor;
    uint32_t n = elapsed / interval;

    if ((elapsed % interval) != 0U)
        n++;

    if (anchor + n * interval <= now)
        n++;

    return anchor + n * interval;
}

static uint32_t next_weekly(uint32_t now)
{
    uint32_t day0 = now - (now % 86400UL);
    uint32_t days_since_epoch = day0 / 86400UL;
    uint8_t today = (uint8_t)((days_since_epoch + 3UL) % 7UL); /* Monday=0 */

    for (uint8_t add = 0; add < 8U; add++)
    {
        uint8_t dow = (uint8_t)((today + add) % 7U);

        if ((m_rule.dow_mask & (1U << dow)) == 0U)
            continue;

        uint32_t candidate =
            day0 + (uint32_t)add * 86400UL + m_rule.arg0;

        if (candidate > now)
            return candidate;
    }

    return 0U;
}

static bool arm_next(void)
{
    /*
     * P0.21 gives NUS/programming absolute priority.
     * This lock is intentionally sticky and is cleared only by reboot.
     */
    if (m_programming_suspend)
    {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        m_state = NRFCLAW_SCHED_SUSPENDED;
        return true;
    }

    if (m_rule.mode == NRFCLAW_SCHEDULE_MANUAL)
    {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        m_state = NRFCLAW_SCHED_DISABLED;
        return true;
    }

    if (m_rule.mode == NRFCLAW_SCHEDULE_BOOT)
    {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        if (!nrfclaw_vm_run_loaded()) {
            m_state = NRFCLAW_SCHED_ERROR;
            return false;
        }
        m_state = NRFCLAW_SCHED_RUNNING;
        return true;
    }

    /*
     * Absolute schedules require a valid wall clock.
     *
     * EVERY is intentionally different: it is a relative periodic rule and
     * must keep working after reset even when no phone/IDE is available to
     * synchronize UTC. Before TIME_SYNC, nrfclaw_rtc_now() returns uptime
     * seconds, so we simply schedule the next occurrence relative to boot.
     *
     * Once TIME_SYNC happens, arm_next() is called again and EVERY resumes
     * using its persisted UTC anchor (arg1), preserving phase when possible.
     */
    bool time_valid =
        nrfclaw_rtc_is_valid();

    if (!time_valid &&
        m_rule.mode != NRFCLAW_SCHEDULE_EVERY)
    {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        m_state = NRFCLAW_SCHED_WAIT_TIME;
        return true;
    }

    uint32_t now =
        nrfclaw_rtc_now();

    uint32_t next = 0U;

    switch (m_rule.mode)
    {
        case NRFCLAW_SCHEDULE_AT:
            if (m_rule.arg0 <= now)
            {
                m_state = NRFCLAW_SCHED_EXPIRED;
                m_next_epoch = 0U;
                nrfclaw_rtc_cancel_alarm();
                return true;
            }

            next =
                m_rule.arg0;
            break;


        case NRFCLAW_SCHEDULE_EVERY:
            if (time_valid)
            {
                /*
                 * Normal synchronized operation: keep the phase based on the
                 * persisted UTC anchor.
                 */
                next =
                    next_every(now);
            }
            else
            {
                /*
                 * Autonomous post-reset operation.
                 *
                 * No absolute clock is available, but EVERY only needs a
                 * relative interval. Start a fresh phase from current uptime.
                 */
                next =
                    now + m_rule.arg0;
            }

            break;


        case NRFCLAW_SCHEDULE_WEEKLY:
            next =
                next_weekly(now);
            break;


        default:
            m_state =
                NRFCLAW_SCHED_ERROR;

            return false;
    }

    if (next == 0U || !nrfclaw_rtc_set_alarm_epoch(next))
    {
        m_state = NRFCLAW_SCHED_ERROR;
        m_next_epoch = 0U;
        return false;
    }

    m_next_epoch = next;
    m_state = NRFCLAW_SCHED_ARMED;

    SEGGER_RTT_printf(
        0,
        "SCHED: armed mode=%u next=%lu\r\n",
        (unsigned)m_rule.mode,
        (unsigned long)m_next_epoch
    );

    return true;
}

void nrfclaw_scheduler_init(void)
{
    m_rule.mode = NRFCLAW_SCHEDULE_MANUAL;
    m_rule.dow_mask = 0U;
    m_rule.reserved = 0U;
    m_rule.arg0 = 0U;
    m_rule.arg1 = 0U;

    m_state = NRFCLAW_SCHED_DISABLED;
    m_next_epoch = 0U;
    m_manual_pause = false;
    m_programming_suspend = false;
}

static bool set_rule_internal(nrfclaw_schedule_t const *rule,
                              bool allow_boot_run)
{
    if (!rule_valid(rule))
        return false;

    m_rule = *rule;
    m_rule.dow_mask &= 0x7FU;
    m_rule.reserved = 0U;
    m_manual_pause = false;

    /*
     * A new upload may change the persistent rule while the device is in
     * programming mode, but it must never start/re-arm that rule now.
     * The new rule becomes active after reboot.
     */
    if (m_programming_suspend)
    {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        m_state = NRFCLAW_SCHED_SUSPENDED;
        return true;
    }

    if (m_rule.mode == NRFCLAW_SCHEDULE_BOOT && !allow_boot_run) {
        nrfclaw_rtc_cancel_alarm();
        m_next_epoch = 0U;
        m_state = NRFCLAW_SCHED_DISABLED;
        return true;
    }

    return arm_next();
}

bool nrfclaw_scheduler_set_rule(nrfclaw_schedule_t const *rule)
{
    return set_rule_internal(rule, true);
}

bool nrfclaw_scheduler_set_rule_deferred(nrfclaw_schedule_t const *rule)
{
    return set_rule_internal(rule, false);
}

void nrfclaw_scheduler_on_time_sync(void)
{
    if (m_programming_suspend)
        return;

    if (m_rule.mode != NRFCLAW_SCHEDULE_MANUAL && !m_manual_pause)
        (void)arm_next();
}

bool nrfclaw_scheduler_on_rtc_event(void)
{
    if (m_programming_suspend)
        return false;

    if (m_state != NRFCLAW_SCHED_ARMED)
        return false;

    m_next_epoch = 0U;

    if (!nrfclaw_vm_run_loaded())
    {
        SEGGER_RTT_WriteString(0, "SCHED ERROR: VM RUN rejected\r\n");
        m_state = NRFCLAW_SCHED_ERROR;
        return true;
    }

    m_state = NRFCLAW_SCHED_RUNNING;
    SEGGER_RTT_WriteString(0, "SCHED: fired -> VM RUN\r\n");
    return true;
}

void nrfclaw_scheduler_pause_for_manual_run(void)
{
    if (m_state == NRFCLAW_SCHED_ARMED)
        nrfclaw_rtc_cancel_alarm();

    m_manual_pause = true;
    m_next_epoch = 0U;

    /*
     * During a NUS programming session this is only an explicit test RUN.
     * Never release the programming scheduler lock.
     */
    if (m_programming_suspend)
        m_state = NRFCLAW_SCHED_SUSPENDED;
}


void nrfclaw_scheduler_suspend_for_programming(void)
{
    /*
     * Sticky until reset. We deliberately do not provide a resume function:
     * the operator programs/tests through NUS and resets to enter application
     * mode again.
     */
    m_programming_suspend = true;
    m_manual_pause = false;

    nrfclaw_rtc_cancel_alarm();

    m_next_epoch = 0U;
    m_state = NRFCLAW_SCHED_SUSPENDED;
}


bool nrfclaw_scheduler_is_programming_suspended(void)
{
    return m_programming_suspend;
}

void nrfclaw_scheduler_process(void)
{
    /*
     * Absolute programming priority: an autonomous schedule can never restart
     * the application VM while P0.21/NUS owns the device.
     */
    if (m_programming_suspend)
    {
        m_state = NRFCLAW_SCHED_SUSPENDED;
        m_next_epoch = 0U;
        return;
    }

    if (m_state != NRFCLAW_SCHED_RUNNING && !m_manual_pause)
        return;

    nrfclaw_vm_state_t vm = nrfclaw_vm_state();

    if (vm == NRFCLAW_VM_READY ||
        vm == NRFCLAW_VM_WAIT_RTC ||
        vm == NRFCLAW_VM_WAIT_HALL ||
        vm == NRFCLAW_VM_WAIT_BATTERY ||
        vm == NRFCLAW_VM_WAIT_LORA ||
        vm == NRFCLAW_VM_WAIT_EVENT)
        return;

    if (m_rule.mode == NRFCLAW_SCHEDULE_AT ||
        m_rule.mode == NRFCLAW_SCHEDULE_BOOT)
    {
        m_state = NRFCLAW_SCHED_EXPIRED;
        m_next_epoch = 0U;
        m_manual_pause = false;
        return;
    }

    m_manual_pause = false;
    (void)arm_next();
}

nrfclaw_scheduler_state_t nrfclaw_scheduler_state(void)
{
    return m_state;
}

nrfclaw_schedule_t nrfclaw_scheduler_rule(void)
{
    return m_rule;
}

uint32_t nrfclaw_scheduler_next_epoch(void)
{
    return m_next_epoch;
}
