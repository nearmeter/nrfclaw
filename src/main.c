#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ndp_key_store.h"
#include <stdint.h>

#include "app_timer.h"
#include "nrf_pwr_mgmt.h"
#include "app_error.h"
#include "nrf_drv_gpiote.h"
#include "SEGGER_RTT.h"

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
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_ble_boot.h"
#include "nrfclaw_state.h"
#include "nrfclaw_factory.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_board_api.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_serial.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_ninalink_lab.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_external_subscription.h"
#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_b55_gate.h"

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
        /* Programming/NUS always owns the device after physical presence. */
        m_programming_pending = true;

        /* P0.21 has priority over the experimental tracking advertiser. */
        nrfclaw_tracking_stop();
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
        return;
    }

    nrfclaw_battery_on_event(p_evt);
    nrfclaw_ds18b20_on_event(p_evt);
    nrfclaw_lis2dh12_on_event(p_evt);
    nrfclaw_vib_auto_on_event(p_evt);
    /* r3.8.11 watchdog polling is an internal driver wake, not a VM/user event. */
    if (p_evt->type == NRFCLAW_EVT_ACCEL_VIBRATION_POLL)
        return;
    nrfclaw_vm_on_event(p_evt);
}

int main(void)
{
    nrfclaw_board_api_init();
    SEGGER_RTT_Init();

    SEGGER_RTT_WriteString(
        0,
        "\r\n========================\r\n"
        "nRFClaw Stage 8.2 runtime\r\n"
        "========================\r\n");

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
    nrfclaw_ble_boot_init();
    nrfclaw_ble_app_init();

#if NRFCLAW_HA_NATIVE_ENABLE
    if (nrfclaw_ble_boot_ndp_enabled()) {
    /*
     * Pack 02 native Home Assistant transport.
     *
     * Advertising is intentionally owned by the existing Application BLE
     * plane. Home Assistant connects to the Application GATT service and
     * exchanges NDP-SESSION frames directly. P0.21 still has priority and
     * temporarily suspends this plane for the validated NUS programming
     * session.
     *
     * Per-board current consumption is NOT assumed here; the interval and
     * TX power are board configuration values and must be measured.
     */
    APP_ERROR_CHECK(
        nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_PERIPHERAL) ==
        NRFCLAW_BLE_APP_OK ? NRF_SUCCESS : NRF_ERROR_INTERNAL);

    APP_ERROR_CHECK(
        nrfclaw_ble_app_adaptive_config(
            NRFCLAW_HA_ADV_FAST_INTERVAL_MS,
            NRFCLAW_HA_ADV_FAST_WINDOW_MS,
            NRFCLAW_HA_ADV_NORMAL_INTERVAL_MS,
            NRFCLAW_HA_ADV_NORMAL_WINDOW_MS,
            NRFCLAW_HA_ADV_SLOW_INTERVAL_MS) ==
        NRFCLAW_BLE_APP_OK ? NRF_SUCCESS : NRF_ERROR_INTERNAL);

    APP_ERROR_CHECK(
        nrfclaw_ble_app_adv_config(
            NRFCLAW_HA_ADV_FAST_INTERVAL_MS,
            NRFCLAW_HA_ADV_TX_POWER_DBM,
            NULL,
            0U) == NRFCLAW_BLE_APP_OK ? NRF_SUCCESS : NRF_ERROR_INTERNAL);

    APP_ERROR_CHECK(
        nrfclaw_ble_app_adv_start() ==
        NRFCLAW_BLE_APP_OK ? NRF_SUCCESS : NRF_ERROR_INTERNAL);
    } else {
        SEGGER_RTT_WriteString(0, "BLE APP: NDP boot advertising DISABLED (P0.21/NUS still available)\r\n");
    }
#endif

    nrfclaw_tracking_init();

    nrfclaw_lora_init();
    lora_irq_init();
    nrfclaw_lora_irq_ready();

    nrfclaw_vm_init();
    nrfclaw_scheduler_init();
    nrfclaw_native_init();
    APP_ERROR_CHECK(nrfclaw_ninalink_lab_init() ? NRF_SUCCESS : NRF_ERROR_INTERNAL);
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
            SEGGER_RTT_WriteString(
                0,
                "FLASH: recovered image rejected by VM validator\r\n"
            );
        }
        else
        {
            /*
             * Program and schedule were authenticated together in the
             * persistent slot. Wall-clock is intentionally invalid after
             * reset, so non-manual rules enter WAIT_TIME here.
             */
            (void)nrfclaw_scheduler_set_rule(
                &persisted_schedule
            );

            /*
             * Stage 8.2:
             *
             * BOOT programs are autonomous by definition. Do not leave them
             * waiting for an explicit NUS RUN after reset.
             *
             * This is intentionally handled here, immediately after recovery,
             * so TRACKING and other BOOT applications work even before wall
             * clock synchronization.
             */
            if (persisted_schedule.mode ==
                NRFCLAW_SCHEDULE_BOOT)
            {
                if (nrfclaw_vm_run_loaded())
                {
                    SEGGER_RTT_WriteString(
                        0,
                        "VM: BOOT program auto-started.\r\n"
                    );
                }
                else
                {
                    SEGGER_RTT_WriteString(
                        0,
                        "VM: BOOT auto-start failed.\r\n"
                    );
                }
            }
        }
    }

    if (nrfclaw_vm_loaded_program_len() != 0U)
    {
        if (nrfclaw_vm_state() == NRFCLAW_VM_READY)
        {
            SEGGER_RTT_WriteString(
                0,
                "VM: persistent BOOT program running.\r\n"
            );
        }
        else
        {
            SEGGER_RTT_WriteString(
                0,
                "VM: persistent program ready.\r\n"
            );
        }
    }
    else
    {
        SEGGER_RTT_WriteString(
            0,
            "VM: STOPPED, no program loaded. Press P0.21 for BLE programming.\r\n"
        );
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

        nrfclaw_ninalink_lab_process();
        nrfclaw_lora_process();
        nrfclaw_ninalink_bridge_process();

        /*
         * NinaLink low-power turnaround invariant:
         * finish TX housekeeping before WAIT_TX evaluates radio idle.
         * Otherwise the MCU may sleep before arming the ACK RX window.
         */
        nrfclaw_ninalink_link_process();
        nrfclaw_b55_gate_process();
        nrfclaw_lora_profile_process();

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
        nrfclaw_ninalink_external_subscription_process();

        nrf_pwr_mgmt_run();
    }
}
