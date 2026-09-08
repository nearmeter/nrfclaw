#include "nrfclaw_system_power.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_native.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_ds18b20.h"
#include "SEGGER_RTT.h"

void nrfclaw_system_minimum_power(void)
{
    /* Stop application activities first so they cannot re-arm peripherals. */
    nrfclaw_tracking_stop();
    nrfclaw_vib_auto_stop();

    (void)nrfclaw_ble_app_adv_stop();
    (void)nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_OFF);

    nrfclaw_native_serial_disable();
    nrfclaw_hall_disable();
    nrfclaw_lis2dh12_disable();
    nrfclaw_lora_sleep();
    nrfclaw_ds18b20_idle_lowpower();

    /* P0.21 is intentionally untouched: nrfclaw_inputs keeps its GPIOTE
     * wake/programming ownership active. */
    SEGGER_RTT_WriteString(0, "POWER R3.8.18b: minimum-power baseline, P0.21 preserved\r\n");
}
