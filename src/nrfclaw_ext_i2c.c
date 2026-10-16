#include "nrfclaw_ext_i2c.h"

#include "nrf_delay.h"
#include "nrf_gpio.h"

#include <string.h>

/*
 * B8.1k8b1 external SHT bus.
 *
 * Software I2C is intentional:
 * - LIS2DH12 keeps its existing hardware TWI1 path;
 * - user may choose arbitrary supported GPIO pins;
 * - short SHT transactions cannot reconfigure/steal the LIS TWI peripheral.
 *
 * No external pull-ups are required for the intended direct-wire SHT use.
 * Released lines use the nRF52 internal GPIO pull-ups.
 */

static void pin_highz(uint8_t pin)
{
    nrf_gpio_cfg(pin,
                 NRF_GPIO_PIN_DIR_INPUT,
                 NRF_GPIO_PIN_INPUT_DISCONNECT,
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_S0S1,
                 NRF_GPIO_PIN_NOSENSE);
}

static void line_release(uint8_t pin)
{
    /*
     * Open-drain HIGH/released state.
     * Keep input connected because we need to sample ACK/data and clock-high.
     */
    nrf_gpio_cfg(pin,
                 NRF_GPIO_PIN_DIR_INPUT,
                 NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_PULLUP,
                 NRF_GPIO_PIN_S0S1,
                 NRF_GPIO_PIN_NOSENSE);
}

static void line_low(uint8_t pin)
{
    /* Never drive HIGH. Set the output latch LOW before changing direction. */
    nrf_gpio_pin_clear(pin);
    nrf_gpio_cfg(pin,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_DISCONNECT,
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_S0S1,
                 NRF_GPIO_PIN_NOSENSE);
}

static void half_delay(void)
{
    nrf_delay_us(NRFCLAW_EXT_I2C_HALF_PERIOD_US);
}

static bool line_is_high(uint8_t pin)
{
    return nrf_gpio_pin_read(pin) != 0U;
}

static bool release_scl_wait(nrfclaw_ext_i2c_t *bus)
{
    uint32_t waited = 0U;

    line_release(bus->scl_pin);

    while (!line_is_high(bus->scl_pin)) {
        if (waited >= NRFCLAW_EXT_I2C_STRETCH_TIMEOUT_US)
            return false;
        nrf_delay_us(1U);
        waited++;
    }

    return true;
}

static void bus_release(nrfclaw_ext_i2c_t *bus)
{
    line_release(bus->sda_pin);
    line_release(bus->scl_pin);
}

void nrfclaw_ext_i2c_idle(nrfclaw_ext_i2c_t *bus)
{
    if (!bus || !bus->configured)
        return;

    /*
     * Since this design intentionally has no external pull-ups, keep the
     * internal pull-ups enabled while the SHT bus is claimed/configured.
     * This prevents SCL/SDA from floating between conversions.
     */
    bus_release(bus);
}

bool nrfclaw_ext_i2c_configure(nrfclaw_ext_i2c_t *bus,
                               uint8_t scl_pin,
                               uint8_t sda_pin)
{
    if (!bus || scl_pin > 31U || sda_pin > 31U || scl_pin == sda_pin)
        return false;

    memset(bus, 0, sizeof(*bus));
    bus->scl_pin = scl_pin;
    bus->sda_pin = sda_pin;
    bus->configured = true;

    nrfclaw_ext_i2c_idle(bus);
    return true;
}

bool nrfclaw_ext_i2c_configure_sht_default(nrfclaw_ext_i2c_t *bus)
{
    return nrfclaw_ext_i2c_configure(
        bus,
        (uint8_t)P_SHT_SCL_DEFAULT,
        (uint8_t)P_SHT_SDA_DEFAULT);
}

void nrfclaw_ext_i2c_deinit(nrfclaw_ext_i2c_t *bus)
{
    if (!bus)
        return;

    if (bus->configured) {
        pin_highz(bus->scl_pin);
        pin_highz(bus->sda_pin);
    }

    memset(bus, 0, sizeof(*bus));
}

static bool start_condition(nrfclaw_ext_i2c_t *bus)
{
    bus_release(bus);

    if (!release_scl_wait(bus))
        return false;

    half_delay();

    if (!line_is_high(bus->sda_pin))
        return false;

    line_low(bus->sda_pin);
    half_delay();
    line_low(bus->scl_pin);
    half_delay();

    return true;
}

static bool repeated_start(nrfclaw_ext_i2c_t *bus)
{
    line_release(bus->sda_pin);
    half_delay();

    if (!release_scl_wait(bus))
        return false;

    half_delay();
    line_low(bus->sda_pin);
    half_delay();
    line_low(bus->scl_pin);
    half_delay();

    return true;
}

static bool stop_condition(nrfclaw_ext_i2c_t *bus)
{
    bool ok = true;

    line_low(bus->sda_pin);
    half_delay();

    if (!release_scl_wait(bus))
        ok = false;

    half_delay();
    line_release(bus->sda_pin);
    half_delay();

    return ok;
}

static bool write_bit(nrfclaw_ext_i2c_t *bus, bool one)
{
    if (one)
        line_release(bus->sda_pin);
    else
        line_low(bus->sda_pin);

    half_delay();

    if (!release_scl_wait(bus))
        return false;

    half_delay();
    line_low(bus->scl_pin);
    half_delay();

    return true;
}

static bool read_bit(nrfclaw_ext_i2c_t *bus, bool *one)
{
    if (!one)
        return false;

    line_release(bus->sda_pin);
    half_delay();

    if (!release_scl_wait(bus))
        return false;

    half_delay();
    *one = line_is_high(bus->sda_pin);
    line_low(bus->scl_pin);
    half_delay();

    return true;
}

static bool write_byte(nrfclaw_ext_i2c_t *bus, uint8_t value)
{
    uint8_t mask;
    bool nack;

    for (mask = 0x80U; mask != 0U; mask >>= 1U) {
        if (!write_bit(bus, (value & mask) != 0U))
            return false;
    }

    if (!read_bit(bus, &nack))
        return false;

    return !nack;
}

static bool read_byte(nrfclaw_ext_i2c_t *bus, uint8_t *value, bool ack)
{
    uint8_t i;
    uint8_t v = 0U;
    bool one;

    if (!value)
        return false;

    for (i = 0U; i < 8U; ++i) {
        if (!read_bit(bus, &one))
            return false;
        v = (uint8_t)((v << 1U) | (one ? 1U : 0U));
    }

    /* ACK is active-low. */
    if (!write_bit(bus, !ack))
        return false;

    *value = v;
    return true;
}

bool nrfclaw_ext_i2c_recover(nrfclaw_ext_i2c_t *bus)
{
    uint8_t i;
    bool ok = true;

    if (!bus || !bus->configured || bus->busy)
        return false;

    bus->busy = true;
    bus_release(bus);

    for (i = 0U; i < 9U; ++i) {
        line_low(bus->scl_pin);
        half_delay();

        if (!release_scl_wait(bus)) {
            ok = false;
            break;
        }

        half_delay();

        if (line_is_high(bus->sda_pin))
            break;
    }

    if (ok)
        ok = stop_condition(bus);

    nrfclaw_ext_i2c_idle(bus);
    bus->busy = false;

    return ok && line_is_high(bus->sda_pin);
}

static bool begin_transaction(nrfclaw_ext_i2c_t *bus)
{
    uint8_t i;

    if (!bus || !bus->configured || bus->busy)
        return false;

    bus->busy = true;
    bus_release(bus);

    if (!line_is_high(bus->sda_pin)) {
        for (i = 0U; i < 9U && !line_is_high(bus->sda_pin); ++i) {
            line_low(bus->scl_pin);
            half_delay();

            if (!release_scl_wait(bus))
                break;

            half_delay();
        }

        (void)stop_condition(bus);
    }

    if (!start_condition(bus)) {
        nrfclaw_ext_i2c_idle(bus);
        bus->busy = false;
        return false;
    }

    return true;
}

static bool finish_transaction(nrfclaw_ext_i2c_t *bus, bool ok)
{
    bool stop_ok = stop_condition(bus);

    /*
     * Keep internal pull-ups enabled because the claimed bus has no
     * external pull-up resistors.
     */
    nrfclaw_ext_i2c_idle(bus);
    bus->busy = false;

    return ok && stop_ok;
}

bool nrfclaw_ext_i2c_write(nrfclaw_ext_i2c_t *bus,
                           uint8_t address7,
                           uint8_t const *data,
                           size_t len)
{
    size_t i;
    bool ok;

    if (address7 > 0x7FU || (len != 0U && !data))
        return false;

    if (!begin_transaction(bus))
        return false;

    ok = write_byte(bus, (uint8_t)(address7 << 1U));

    for (i = 0U; ok && i < len; ++i)
        ok = write_byte(bus, data[i]);

    return finish_transaction(bus, ok);
}

bool nrfclaw_ext_i2c_read(nrfclaw_ext_i2c_t *bus,
                          uint8_t address7,
                          uint8_t *data,
                          size_t len)
{
    size_t i;
    bool ok;

    if (address7 > 0x7FU || (len != 0U && !data))
        return false;

    if (!begin_transaction(bus))
        return false;

    ok = write_byte(bus, (uint8_t)((address7 << 1U) | 1U));

    for (i = 0U; ok && i < len; ++i)
        ok = read_byte(bus, &data[i], i + 1U < len);

    return finish_transaction(bus, ok);
}

bool nrfclaw_ext_i2c_write_read(nrfclaw_ext_i2c_t *bus,
                                uint8_t address7,
                                uint8_t const *write_data,
                                size_t write_len,
                                uint8_t *read_data,
                                size_t read_len)
{
    size_t i;
    bool ok;

    if (address7 > 0x7FU ||
        (write_len != 0U && !write_data) ||
        (read_len != 0U && !read_data))
        return false;

    if (!begin_transaction(bus))
        return false;

    ok = write_byte(bus, (uint8_t)(address7 << 1U));

    for (i = 0U; ok && i < write_len; ++i)
        ok = write_byte(bus, write_data[i]);

    if (ok && read_len != 0U)
        ok = repeated_start(bus);

    if (ok && read_len != 0U)
        ok = write_byte(bus, (uint8_t)((address7 << 1U) | 1U));

    for (i = 0U; ok && i < read_len; ++i)
        ok = read_byte(bus, &read_data[i], i + 1U < read_len);

    return finish_transaction(bus, ok);
}
