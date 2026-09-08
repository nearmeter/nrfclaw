#include "nrfclaw_native.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_board.h"
#include "nrfclaw_serial.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrf_gpio.h"


static void gpio_release_to_default(uint8_t pin)
{
    /*
     * D20 has an external 4.7 kOhm pull-up for optional DS18B20 use.
     * Keep it high impedance to avoid wasting current through that resistor.
     */
    if (pin == P_DS18) {
        nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_NOPULL);
        return;
    }

    /*
     * Shared/free user GPIOs default to INPUT + PULLDOWN.
     * This gives a deterministic LOW without actively driving the pin.
     */
    switch (pin) {
        case P_HALL1:
        case P_HALL2:
        case P_FREE_D15:
        case P_FREE_D16:
        case P_FREE_A29:
        case P_FREE_A30:
        case P_FREE_A31:
            nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLDOWN);
            break;

        /*
         * D18 belongs to LIS2DH12 when the accelerometer is present.
         * When absent it may be used as GPIO, but the LIS driver owns the
         * release/probe state, so do not override it here at boot.
         */
        case P_LIS_SDA:
            if (!nrfclaw_lis2dh12_present())
                nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLDOWN);
            break;

        default:
            break;
    }
}

static bool gpio_allowed(uint8_t pin)
{
    if (nrfclaw_serial_owns_pin(pin)) return false;
    switch (pin) {
        case P_HALL1:
        case P_HALL2:
            return !nrfclaw_hall_active();

        case P_DS18:
            return !nrfclaw_ds18b20_present();

        case P_FREE_D15:
        case P_FREE_D16:
        case P_FREE_A29:
        case P_FREE_A30:
        case P_FREE_A31:
            return true;

        /* D18 is a general-purpose candidate only when LIS2DH12 is absent. */
        case P_LIS_SDA:
            return !nrfclaw_lis2dh12_present();

        default:
            return false;
    }
}

void nrfclaw_native_init(void)
{
    nrfclaw_serial_init();

    /*
     * Shared/general-purpose GPIO default state.
     *
     * D11/D14 are optional Hall/Reed inputs and general-purpose GPIOs.
     * D15/D16/A29/A30/A31 are exposed general-purpose GPIOs.
     * All start as input + pulldown so they read a deterministic LOW and
     * never float around intermediate voltages.
     */
    gpio_release_to_default(P_HALL1);
    gpio_release_to_default(P_HALL2);
    gpio_release_to_default(P_FREE_D15);
    gpio_release_to_default(P_FREE_D16);
    gpio_release_to_default(P_FREE_A29);
    gpio_release_to_default(P_FREE_A30);
    gpio_release_to_default(P_FREE_A31);

    /*
     * D20 is shared with an optional DS18B20 and has an external 4.7 kOhm
     * pull-up on the PCB. Keep it input/high-Z; measuring HIGH is expected.
     */
    gpio_release_to_default(P_DS18);

    /* Stage 10.2.3: automatic, one-time 1-Wire presence probe. */
    nrfclaw_ds18b20_init();

    /*
     * Probe LIS2DH12 last. If it is absent, the LIS driver releases its pins.
     * D18 can then fall back to the same deterministic GPIO LOW state.
     */
    nrfclaw_lis2dh12_init();

    if (!nrfclaw_lis2dh12_present())
        gpio_release_to_default(P_LIS_SDA);
}

bool nrfclaw_native_capability_supported(uint8_t capability)
{
    /*
     * NDP v1 discovery semantics: supported means the selected board/firmware
     * can provide the capability. It MUST NOT depend on whether a runtime
     * feature (Hall, serial, tracking, Vib Auto) is currently enabled.
     * Optional probed peripherals still require the physical device to be
     * present, so HA does not create dead sensor entities.
     */
    switch (capability) {
        case NRFCLAW_CAP_GPIO:     return true;
        case NRFCLAW_CAP_BATTERY:  return NRFCLAW_BOARD_HAS_BATTERY != 0;
        case NRFCLAW_CAP_DS18B20:  return (NRFCLAW_BOARD_HAS_DS18B20 != 0) && nrfclaw_ds18b20_present();
        case NRFCLAW_CAP_ACCEL:    return (NRFCLAW_BOARD_HAS_LIS2DH12 != 0) && nrfclaw_lis2dh12_present();
        case NRFCLAW_CAP_LORA:     return NRFCLAW_BOARD_HAS_LORA != 0;
        case NRFCLAW_CAP_BLE_APP:  return NRFCLAW_HA_NATIVE_ENABLE != 0;
        case NRFCLAW_CAP_RTC:      return true;
        case NRFCLAW_CAP_HALL:     return NRFCLAW_BOARD_HAS_HALL != 0;
        case NRFCLAW_CAP_STATE:    return true;
        case NRFCLAW_CAP_SERIAL:   return true;
        case NRFCLAW_CAP_TRACKING: return true;
        case NRFCLAW_CAP_VIB_HEALTH:
            return (nrfclaw_vib_health_build_gate() == 1U) &&
                   nrfclaw_vib_health_supported();
        case NRFCLAW_CAP_VIB_AUTO:
            return (nrfclaw_vib_auto_build_gate() == 1U) &&
                   nrfclaw_vib_auto_supported();
        default:
            return false;
    }
}

bool nrfclaw_native_capability_active(uint8_t capability)
{
    switch (capability) {
        case NRFCLAW_CAP_GPIO:     return true;
        case NRFCLAW_CAP_BATTERY:  return nrfclaw_native_capability_supported(capability);
        case NRFCLAW_CAP_DS18B20:  return nrfclaw_ds18b20_present();
        case NRFCLAW_CAP_ACCEL:    return nrfclaw_lis2dh12_present();
        case NRFCLAW_CAP_LORA:     return nrfclaw_native_capability_supported(capability);
        case NRFCLAW_CAP_BLE_APP:  return nrfclaw_native_capability_supported(capability);
        case NRFCLAW_CAP_RTC:      return true;
        case NRFCLAW_CAP_HALL:     return nrfclaw_hall_active();
        case NRFCLAW_CAP_STATE:    return true;
        case NRFCLAW_CAP_SERIAL:   return nrfclaw_serial_active();
        case NRFCLAW_CAP_TRACKING: return nrfclaw_tracking_active();
        case NRFCLAW_CAP_VIB_HEALTH:
            return nrfclaw_native_capability_supported(capability);
        case NRFCLAW_CAP_VIB_AUTO: {
            nrfclaw_vib_auto_status_t st;
            if (!nrfclaw_native_capability_supported(capability)) return false;
            nrfclaw_vib_auto_get_status(&st);
            return st.enabled;
        }
        default:
            return false;
    }
}

bool nrfclaw_native_capability_available(uint8_t capability)
{
    /* Backward-compatible runtime predicate used by the VM / legacy commands. */
    return nrfclaw_native_capability_active(capability);
}

nrfclaw_native_status_t nrfclaw_native_hall_enable(bool pullup)
{
    return nrfclaw_hall_enable(pullup)
        ? NRFCLAW_NATIVE_OK
        : NRFCLAW_NATIVE_ERROR;
}

nrfclaw_native_status_t nrfclaw_native_hall_configure(nrfclaw_hall_config_t const *cfg)
{
    return nrfclaw_hall_configure(cfg) ? NRFCLAW_NATIVE_OK : NRFCLAW_NATIVE_ERROR;
}

void nrfclaw_native_hall_disable(void)
{
    nrfclaw_hall_disable();
}

bool nrfclaw_native_ds18_active(void)
{
    return nrfclaw_ds18b20_present();
}

nrfclaw_native_status_t nrfclaw_native_gpio_write(uint8_t pin, bool high)
{
    if (!gpio_allowed(pin))
        return NRFCLAW_NATIVE_BUSY;

    nrf_gpio_cfg_output(pin);
    if (high) nrf_gpio_pin_set(pin);
    else      nrf_gpio_pin_clear(pin);
    return NRFCLAW_NATIVE_OK;
}

nrfclaw_native_status_t nrfclaw_native_gpio_read(uint8_t pin, uint32_t *value)
{
    if (!value)
        return NRFCLAW_NATIVE_BAD_ARG;
    if (!gpio_allowed(pin))
        return NRFCLAW_NATIVE_BUSY;

    /*
     * Restore the board-defined safe input state before reading.
     * D20 remains high-Z because of its external pull-up; the other exposed
     * general-purpose pins use pulldown so they do not float.
     */
    gpio_release_to_default(pin);

    *value = nrf_gpio_pin_read(pin) ? 1U : 0U;
    return NRFCLAW_NATIVE_OK;
}

nrfclaw_native_status_t nrfclaw_native_accel_xyz(int16_t *x, int16_t *y, int16_t *z)
{
    if (!x || !y || !z)
        return NRFCLAW_NATIVE_BAD_ARG;

    nrfclaw_lis2dh12_xyz_t xyz;
    if (!nrfclaw_lis2dh12_read_xyz(&xyz))
        return NRFCLAW_NATIVE_UNAVAILABLE;

    *x = xyz.x_mg;
    *y = xyz.y_mg;
    *z = xyz.z_mg;
    return NRFCLAW_NATIVE_OK;
}

nrfclaw_native_status_t nrfclaw_native_accel_motion(uint16_t threshold_mg,
                                                     uint8_t duration_ticks)
{
    return nrfclaw_lis2dh12_enable_motion(threshold_mg, duration_ticks)
        ? NRFCLAW_NATIVE_OK
        : NRFCLAW_NATIVE_UNAVAILABLE;
}

nrfclaw_native_status_t nrfclaw_native_accel_configure(const nrfclaw_accel_config_t *cfg)
{
    if (!cfg) return NRFCLAW_NATIVE_BAD_ARG;
    return nrfclaw_lis2dh12_configure(cfg) ? NRFCLAW_NATIVE_OK : NRFCLAW_NATIVE_BAD_ARG;
}

static bool serial_pin_claimable(uint8_t pin){switch(pin){case P_HALL1:case P_HALL2:return !nrfclaw_hall_active();case P_DS18:return !nrfclaw_ds18b20_present();case P_FREE_D15:case P_FREE_D16:case P_FREE_A29:case P_FREE_A30:case P_FREE_A31:return true;case P_LIS_SDA:return !nrfclaw_lis2dh12_present();default:return false;}}
nrfclaw_native_status_t nrfclaw_native_serial_enable(uint8_t tx,uint8_t rx,uint32_t baud){if(tx==rx)return NRFCLAW_NATIVE_BAD_ARG;nrfclaw_serial_disable();if(!serial_pin_claimable(tx)||!serial_pin_claimable(rx))return NRFCLAW_NATIVE_BUSY;return nrfclaw_serial_enable(tx,rx,baud)?NRFCLAW_NATIVE_OK:NRFCLAW_NATIVE_BAD_ARG;}
nrfclaw_native_status_t nrfclaw_native_serial_enable_economy(uint8_t tx,uint8_t rx,uint32_t baud){if(tx==rx)return NRFCLAW_NATIVE_BAD_ARG;nrfclaw_serial_disable();if(!serial_pin_claimable(tx)||!serial_pin_claimable(rx))return NRFCLAW_NATIVE_BUSY;return nrfclaw_serial_enable_economy(tx,rx,baud)?NRFCLAW_NATIVE_OK:NRFCLAW_NATIVE_BAD_ARG;}
void nrfclaw_native_serial_disable(void){nrfclaw_serial_disable();}
nrfclaw_native_status_t nrfclaw_native_serial_write(uint8_t const*d,uint16_t n){if(!nrfclaw_serial_active())return NRFCLAW_NATIVE_UNAVAILABLE;return nrfclaw_serial_write(d,n)?NRFCLAW_NATIVE_OK:NRFCLAW_NATIVE_ERROR;}
nrfclaw_native_status_t nrfclaw_native_serial_read(uint32_t*v){uint8_t b;if(!v)return NRFCLAW_NATIVE_BAD_ARG;if(!nrfclaw_serial_active())return NRFCLAW_NATIVE_UNAVAILABLE;if(!nrfclaw_serial_read_byte(&b))return NRFCLAW_NATIVE_BUSY;*v=b;return NRFCLAW_NATIVE_OK;}
