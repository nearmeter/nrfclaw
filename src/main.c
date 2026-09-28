#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ndp_key_store.h"
#include <stdint.h>

#include "app_timer.h"
#include "nrf_pwr_mgmt.h"
#include "app_error.h"
#include "nrf_drv_gpiote.h"
#include "nrf_gpio.h"
#include "nrfclaw_event.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_ble.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_vm.h"
#include "nrfclaw_board.h"
#include "nrfclaw_battery.h"
#include "nrfclaw_flash.h"
#include "nrfclaw_scheduler.h"
#include "nrfclaw_native.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_direct_adv.h"
#include "nrfclaw_direct_sensor_control.h"
#include "nrfclaw_direct_hall_control.h"
#include "nrfclaw_ble_boot.h"
#include "nrfclaw_ha_role.h"
#include "nrfclaw_ha_ninalink_node.h"
#include "nrfclaw_state.h"
#include "nrfclaw_factory.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_board_api.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_capability.h"
#include "nrfclaw_serial.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_external_subscription.h"
#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_ninalink_event_router.h"
#include "nrfclaw_ninalink_network.h"
#include "nrfclaw_ninalink_telemetry.h"

#if NRFCLAW_BOARD_HAS_LORA
static void lora_dio_handler(nrf_drv_gpiote_pin_t pin,
                             nrf_gpiote_polarity_t action)
{
    (void)pin;
    (void)action;

    nrf_drv_gpiote_in_event_disable(P_LORA_DIO1);

    nrfclaw_event_t evt = {
        .type = NRFCLAW_EVT_LORA_DIO1
    };

    (void)nrfclaw_event_push_isr(&evt);
}

static void lora_irq_init(void)
{
    nrf_drv_gpiote_in_config_t cfg =
        GPIOTE_CONFIG_IN_SENSE_LOTOHI(true);

    cfg.pull = NRF_GPIO_PIN_NOPULL;

    APP_ERROR_CHECK(
        nrf_drv_gpiote_in_init(
            P_LORA_DIO1,
            &cfg,
            lora_dio_handler));

    nrf_drv_gpiote_in_event_disable(P_LORA_DIO1);
}
#endif /* NRFCLAW_BOARD_HAS_LORA */

static bool m_programming_pending;

static void open_programming_now(void)
{
    nrfclaw_ble_programming_open(NRFCLAW_BLE_DISCOVERY_TIMEOUT_S);
    m_programming_pending=false;
}

static void dispatch(nrfclaw_event_t const *p_evt)
{
    if (!p_evt)
        return;

    if (p_evt->type == NRFCLAW_EVT_BUTTON) {
        /*
         * B7.6f2k3a: GPIOTE/SENSE can leave a transient/stale BUTTON event
         * queued across early boot. P0.21 is our physical-presence gate, so
         * verify that it is still asserted LOW before allowing programming
         * mode to preempt an autonomous BOOT program. A released/HIGH pin
         * must never stop the VM or steal the shared BLE advertiser.
         */
        if (nrf_gpio_pin_read(P_BUTTON) != 0U)
        {
            ((void)0);
            return;
        }

        /* Programming/NUS always owns the device after physical presence. */
        m_programming_pending = true;

        /* P0.21 has priority over the experimental tracking advertiser. */
        nrfclaw_tracking_stop();
        nrfclaw_ninalink_telemetry_stop();
        nrfclaw_ninalink_event_router_reset();
        nrfclaw_ha_ninalink_node_stop();
        nrfclaw_ninalink_bridge_stop();
        nrfclaw_ble_app_suspend();

        /*
         * Keep a BOOT Event-VM alive only when it is sleeping in WAIT_EVENT.
         * This allows NUS debug commands such as EVENT_INJECT to wake the
         * application while programming mode owns BLE. Any actively running
         * or peripheral-waiting VM is still stopped immediately.
         */
        if (nrfclaw_vm_state() != NRFCLAW_VM_WAIT_EVENT)
        {
            (void)nrfclaw_vm_stop();
        }

        /*
         * NUS owns autonomous scheduling until reboot. BOOT/EVERY/AT/WEEKLY
         * cannot restart the VM behind the programming session.
         */
        nrfclaw_scheduler_suspend_for_programming();

        /* If Application BLE had a live link, wait for APP_DISCONNECTED. */
        if (!nrfclaw_ble_app_connected())
            open_programming_now();
    }
    else if (p_evt->type == NRFCLAW_EVT_APP_DISCONNECTED) {
        if (m_programming_pending)
            open_programming_now();
    }
    else if (p_evt->type == NRFCLAW_EVT_RTC) {
        if (nrfclaw_scheduler_on_rtc_event())
            return;
    }
    else if (p_evt->type == NRFCLAW_EVT_LORA_DIO1) {
#if NRFCLAW_BOARD_HAS_LORA
        if (nrfclaw_lora_on_dio1_event()) {
            nrfclaw_event_t tx_done = {
                .type = NRFCLAW_EVT_LORA_TX_DONE,
                .arg0 = 0U,
                .arg1 = 0U
            };
            nrfclaw_vm_on_event(&tx_done);
        } else if (nrfclaw_lora_rx_ready()) {
            nrfclaw_event_t rx_done = {
                .type = NRFCLAW_EVT_LORA_RX_DONE,
                .arg0 = 0U,
                .arg1 = 0U
            };
            nrfclaw_vm_on_event(&rx_done);
        }
#endif
        return;
    }

    nrfclaw_battery_on_event(p_evt);
    nrfclaw_ds18b20_on_event(p_evt);
    nrfclaw_temperature_on_event(p_evt);
    nrfclaw_ninalink_telemetry_on_event(p_evt);
    nrfclaw_ha_ninalink_node_on_event(p_evt);
    if (p_evt->type == NRFCLAW_EVT_ACCEL_MOTION)
        nrfclaw_capability_motion_latch_set();
    nrfclaw_lis2dh12_on_event(p_evt);
    /* B7.6f2l6a raw INT1 qualification is driver-internal.  The driver may
     * enqueue a validated ACCEL_TAP; never expose the raw edge to VM/HA. */
    if (p_evt->type == NRFCLAW_EVT_ACCEL_INT1_RAW)
        return;
    nrfclaw_vib_auto_on_event(p_evt);
    /* r3.8.11 watchdog polling is an internal driver wake, not a VM/user event. */
    if (p_evt->type == NRFCLAW_EVT_ACCEL_VIBRATION_POLL)
        return;
    nrfclaw_direct_adv_on_event(p_evt);
    nrfclaw_ninalink_event_router_on_event(p_evt);
    nrfclaw_vm_on_event(p_evt);
}

int main(void)
{
    nrfclaw_board_api_init();
    ((void)0);

    ((void)0);

    APP_ERROR_CHECK(app_timer_init());
    APP_ERROR_CHECK(nrf_pwr_mgmt_init());

    nrfclaw_event_init();
    m_programming_pending = false;
    nrfclaw_battery_init();
    nrfclaw_rtc_init();
    nrfclaw_inputs_init();

    nrfclaw_ndp_key_store_init();
    nrfclaw_ndp_access_init();
    nrfclaw_ble_init();

    /* r3.8.10: state journal must be scanned before deciding whether the
     * Application/NDP plane is allowed to advertise at boot. The physical
     * P0.21/NUS plane remains initialized regardless of this setting. */
    nrfclaw_state_init();
    nrfclaw_ninalink_network_init(); /* B7.6f2l1 persistent NinaLink network ID */
    nrfclaw_ble_boot_init();
    nrfclaw_ble_app_init();

/* B7.6f: HA transport role is applied only after BLE, LoRa, native
     * capability and NinaLink services are initialized below. Fresh devices
     * stay silent on Application/NDP until a role is explicitly configured. */


    nrfclaw_tracking_init();

#if NRFCLAW_BOARD_HAS_LORA
    nrfclaw_lora_init();
    lora_irq_init();
    nrfclaw_lora_irq_ready();
#endif

    nrfclaw_vm_init();
    nrfclaw_scheduler_init();
    nrfclaw_native_init();
    nrfclaw_direct_sensor_control_init(); /* Direct event mode + sensitivity preset */
    nrfclaw_direct_hall_control_init();   /* B7.6f2k3 saved Direct Hall preset */
    nrfclaw_direct_adv_init(); /* B7.6d/e, B7.6f v2 envelope */
    if (!nrfclaw_ha_role_apply_runtime(nrfclaw_ha_role_get())) {
        ((void)0);
    }
    nrfclaw_vib_health_init();
    nrfclaw_vib_auto_init();
    nrfclaw_factory_init();

    /*
     * Stage 4 persistent program recovery.
     *
     * Both slots are scanned. Only a COMMITTED header with a valid CRC is
     * considered. If both are valid, the newest generation wins.
     */
    nrfclaw_flash_init();

    uint8_t const *persisted_program = NULL;
    uint16_t persisted_len = 0U;
    nrfclaw_schedule_t persisted_schedule;

    if (nrfclaw_flash_get_latest(
            &persisted_program,
            &persisted_len,
            &persisted_schedule))
    {
        if (!nrfclaw_vm_install_program(
                persisted_program,
                persisted_len))
        {
            ((void)0);
        }
        else
        {
            /*
             * Program and schedule were authenticated together in the
             * persistent slot. Wall-clock is intentionally invalid after
             * reset, so non-manual rules enter WAIT_TIME here.
             */
            /*
             * The scheduler owns BOOT startup. set_rule() -> arm_next()
             * already calls nrfclaw_vm_run_loaded() for BOOT rules. Do not
             * call it a second time here: the second call is correctly
             * rejected because the VM is already READY and used to produce
             * the misleading "BOOT auto-start failed" diagnostic.
             */
            bool const schedule_ok = nrfclaw_scheduler_set_rule(
                &persisted_schedule
            );

            if (persisted_schedule.mode ==
                NRFCLAW_SCHEDULE_BOOT)
            {
                if (schedule_ok &&
                    nrfclaw_vm_state() == NRFCLAW_VM_READY)
                {
                    ((void)0);
                }
                else
                {
                    ((void)0);
                }
            }
        }
    }

    if (nrfclaw_vm_loaded_program_len() != 0U)
    {
        if (nrfclaw_vm_state() == NRFCLAW_VM_READY)
        {
            ((void)0);
        }
        else
        {
            ((void)0);
        }
    }
    else
    {
        ((void)0);
    }

    for (;;) {
        /* R3.8.18d: service GPIO->UARTE wake before VM/event dispatch so the
         * wake preamble buys the maximum possible setup time. */
        nrfclaw_serial_process();

        nrfclaw_event_t evt;

        while (nrfclaw_event_pop(&evt))
            dispatch(&evt);

        nrfclaw_vm_tick();

        nrfclaw_scheduler_process();

#if NRFCLAW_BOARD_HAS_LORA
        nrfclaw_lora_process();
#endif
        nrfclaw_ninalink_bridge_process();

        /*
         * NinaLink low-power turnaround invariant:
         * finish TX housekeeping before WAIT_TX evaluates radio idle.
         * Otherwise the MCU may sleep before arming the ACK RX window.
         */
        nrfclaw_ninalink_link_process();
        nrfclaw_ninalink_event_router_process();
        nrfclaw_ninalink_telemetry_process();
        nrfclaw_ha_ninalink_node_process();
#if NRFCLAW_BOARD_HAS_LORA
        nrfclaw_lora_profile_process();
#endif

        /*
         * Stage 8.2 autonomous TRACKING:
         * - commits master-seed/config atomically to 0x74000/0x75000;
         * - handles deferred key rotation outside app_timer callbacks;
         * - reconfigures the shared advertising handle after rotation.
         */
        nrfclaw_tracking_process();

        /* SoftDevice-driven atomic Flash writer. */
        nrfclaw_flash_process();
        nrfclaw_ndp_key_store_process();
        nrfclaw_state_process();
        nrfclaw_factory_process();
        nrfclaw_vib_auto_process();

        nrfclaw_ble_process();

        /* Pack 02 R2G: deferred/retried Application advertising restart. */
        nrfclaw_ble_app_process();
        nrfclaw_direct_adv_process();
        nrfclaw_ninalink_external_subscription_process();

        nrf_pwr_mgmt_run();
    }
}
