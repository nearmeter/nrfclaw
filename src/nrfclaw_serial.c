#include "nrfclaw_serial.h"

#include "nrfclaw_board.h"
#include "nrfclaw_event.h"

#include "nrf_drv_uart.h"
#include "nrf_drv_gpiote.h"
#include "app_timer.h"
#include "app_error.h"
#include "nrf_gpio.h"
#include "sdk_errors.h"
#include "nrf_delay.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>


/*
 * --------------------------------------------------------------------------
 * Stage 8.1 SERIAL capability
 * --------------------------------------------------------------------------
 *
 * - Inactive at boot.
 * - Default logical mapping:
 *       TX = D15
 *       RX = D16
 *       57600 8N1
 *
 * - Pins are not claimed until SERIAL_ENABLE / SERIAL_CONFIG.
 * - When disabled, pins return to their board-safe GPIO state.
 * - RX is interrupt driven through nrf_drv_uart / nrfx.
 *
 * IMPORTANT:
 * Do not define UARTE0_UART0_IRQHandler() here.
 * The Nordic driver / nrfx_prs owns the IRQ vector.
 * --------------------------------------------------------------------------
 */

#define SERIAL_UART_INSTANCE 0


static const nrf_drv_uart_t m_uart =
    NRF_DRV_UART_INSTANCE(SERIAL_UART_INSTANCE);


static volatile uint8_t m_rx_fifo[
    NRFCLAW_SERIAL_RX_BUFFER_SIZE
];

static volatile uint8_t m_rx_head;
static volatile uint8_t m_rx_tail;

static uint8_t m_rx_byte;

/*
 * TX owns its own buffer because nrf_drv_uart_tx() is asynchronous.
 * This decouples an in-flight transmission from VM bytecode memory.
 */
static uint8_t m_tx_buffer[64U];
static volatile bool m_tx_busy;

/* R3.8.18c framed RX state. UART remains byte/IRQ based, while the frame
 * detector lets the VM consume complete chunks without polling. */
APP_TIMER_DEF(m_rx_idle_timer);
static volatile bool m_frame_waiting;
static volatile bool m_frame_ready;
static volatile uint8_t m_frame_len;
static volatile nrfclaw_serial_frame_mode_t m_frame_mode;
static volatile uint16_t m_frame_param;


static bool     m_active;
/* R3.8.18d: continuous remains the compatibility default. Economy mode keeps
 * UARTE completely uninitialized while idle and uses low-accuracy GPIOTE
 * PORT/SENSE on RX to wake the MCU. The wake byte/preamble is sacrificial. */
static bool     m_economy_mode;
static bool     m_driver_initialized;
static volatile bool m_wake_pending;
static volatile bool m_sleep_pending;
static bool     m_wake_configured;
static uint8_t  m_tx_pin;
static uint8_t  m_rx_pin;
static uint32_t m_baud;

static void uart_event_handler(nrf_drv_uart_event_t * p_event, void * p_context);

static void serial_frame_complete(uint8_t len)
{
    if (!m_frame_waiting || len == 0U) return;
    m_frame_waiting = false;
    m_frame_ready = true;
    m_frame_len = len;
    if (m_economy_mode) m_sleep_pending = true;
    (void)app_timer_stop(m_rx_idle_timer);
    nrfclaw_event_t evt = { NRFCLAW_EVT_SERIAL_FRAME, len, 0U };
    (void)nrfclaw_event_push_isr(&evt);
}

static void serial_idle_timer_handler(void *ctx)
{
    (void)ctx;
    if (m_frame_waiting && m_frame_mode == NRFCLAW_SERIAL_FRAME_IDLE) {
        uint8_t n = nrfclaw_serial_available();
        if (n != 0U) serial_frame_complete(n);
    }
}


/*
 * --------------------------------------------------------------------------
 * GPIO release policy
 * --------------------------------------------------------------------------
 *
 * D20 has an external 4.7 kOhm pull-up for 1-Wire/DS18B20, therefore it
 * returns to high impedance.
 *
 * Other user-exposed general-purpose pins return to INPUT + PULLDOWN,
 * matching the Stage 7/8 low-power GPIO policy.
 */
static void release_pin(
    uint8_t pin
)
{
    if (pin == P_DS18)
    {
        nrf_gpio_cfg_input(
            pin,
            NRF_GPIO_PIN_NOPULL
        );
    }
    else
    {
        nrf_gpio_cfg_input(
            pin,
            NRF_GPIO_PIN_PULLDOWN
        );
    }
}


/*
 * --------------------------------------------------------------------------
 * Baud conversion
 * --------------------------------------------------------------------------
 */
static nrf_uart_baudrate_t baud_to_sdk(
    uint32_t baud
)
{
    switch (baud)
    {
        case 1200UL:
            return NRF_UART_BAUDRATE_1200;

        case 2400UL:
            return NRF_UART_BAUDRATE_2400;

        case 4800UL:
            return NRF_UART_BAUDRATE_4800;

        case 9600UL:
            return NRF_UART_BAUDRATE_9600;

        case 14400UL:
            return NRF_UART_BAUDRATE_14400;

        case 19200UL:
            return NRF_UART_BAUDRATE_19200;

        case 28800UL:
            return NRF_UART_BAUDRATE_28800;

        case 38400UL:
            return NRF_UART_BAUDRATE_38400;

        case 57600UL:
            return NRF_UART_BAUDRATE_57600;

        case 76800UL:
            return NRF_UART_BAUDRATE_76800;

        case 115200UL:
            return NRF_UART_BAUDRATE_115200;

        case 230400UL:
            return NRF_UART_BAUDRATE_230400;

        case 250000UL:
            return NRF_UART_BAUDRATE_250000;

        case 460800UL:
            return NRF_UART_BAUDRATE_460800;

        case 921600UL:
            return NRF_UART_BAUDRATE_921600;

        case 1000000UL:
            return NRF_UART_BAUDRATE_1000000;

        default:
            return (nrf_uart_baudrate_t)0;
    }
}



/* --------------------------------------------------------------------------
 * R3.8.18d economy wake support
 * -------------------------------------------------------------------------- */
static void serial_wake_handler(nrf_drv_gpiote_pin_t pin,
                                nrf_gpiote_polarity_t action)
{
    (void)action;
    if (!m_active || !m_economy_mode || pin != m_rx_pin) return;
    /* Disable immediately so a noisy/preamble stream cannot queue a wake storm.
     * Driver initialization is deferred to main context by serial_process(). */
    nrf_drv_gpiote_in_event_disable(m_rx_pin);
    m_wake_pending = true;
}

static void serial_wake_release(void)
{
    if (!m_wake_configured) return;
    nrf_drv_gpiote_in_event_disable(m_rx_pin);
    nrf_drv_gpiote_in_uninit(m_rx_pin);
    m_wake_configured = false;
}

static bool serial_wake_arm(void)
{
    if (!m_active || !m_economy_mode || m_driver_initialized) return false;
    if (m_wake_configured) {
        nrf_drv_gpiote_in_event_enable(m_rx_pin, true);
        return true;
    }
    if (!nrf_drv_gpiote_is_init()) {
        ret_code_t rc = nrf_drv_gpiote_init();
        if (rc != NRF_SUCCESS && rc != NRF_ERROR_INVALID_STATE) return false;
    }
    /* UART idle is HIGH. Low-accuracy GPIOTE uses GPIO SENSE/PORT and is the
     * low-power path; a HITOLO edge from the wake preamble wakes System ON. */
    nrf_drv_gpiote_in_config_t cfg = GPIOTE_CONFIG_IN_SENSE_HITOLO(false);
    cfg.pull = NRF_GPIO_PIN_PULLUP;
    ret_code_t rc = nrf_drv_gpiote_in_init(m_rx_pin, &cfg, serial_wake_handler);
    if (rc != NRF_SUCCESS) return false;
    m_wake_configured = true;
    nrf_drv_gpiote_in_event_enable(m_rx_pin, true);
    return true;
}

static bool serial_driver_start_rx(void)
{
    if (!m_active || m_driver_initialized) return m_driver_initialized;
    nrf_uart_baudrate_t sdk_baud = baud_to_sdk(m_baud);
    if ((uint32_t)sdk_baud == 0U) return false;

    serial_wake_release();

    nrf_drv_uart_config_t config = NRF_DRV_UART_DEFAULT_CONFIG;
    config.pseltxd = m_tx_pin;
    config.pselrxd = m_rx_pin;
    config.pselcts = NRF_UART_PSEL_DISCONNECTED;
    config.pselrts = NRF_UART_PSEL_DISCONNECTED;
    config.baudrate = sdk_baud;
    config.hwfc = NRF_UART_HWFC_DISABLED;
    config.parity = NRF_UART_PARITY_EXCLUDED;
    config.interrupt_priority = 7U;

    ret_code_t err = nrf_drv_uart_init(&m_uart, &config, uart_event_handler);
    if (err != NRF_SUCCESS) return false;
    m_driver_initialized = true;

    /* Keep RX defined HIGH if the external transmitter is disconnected. */
    nrf_gpio_cfg_input(m_rx_pin, NRF_GPIO_PIN_PULLUP);

    err = nrf_drv_uart_rx(&m_uart, &m_rx_byte, 1U);
    if (err != NRF_SUCCESS) {
        nrf_drv_uart_uninit(&m_uart);
        m_driver_initialized = false;
        return false;
    }
    return true;
}

static void serial_driver_stop(void)
{
    if (!m_driver_initialized) return;
    nrf_drv_uart_rx_abort(&m_uart);
    nrf_drv_uart_tx_abort(&m_uart);
    nrf_drv_uart_uninit(&m_uart);
    m_driver_initialized = false;
    m_tx_busy = false;
}

/*
 * --------------------------------------------------------------------------
 * UART callback
 * --------------------------------------------------------------------------
 *
 * nrf_drv_uart owns UARTE0_UART0_IRQHandler through nrfx.
 */
static void uart_event_handler(
    nrf_drv_uart_event_t * p_event,
    void * p_context
)
{
    (void)p_context;


    if (p_event == NULL)
    {
        return;
    }


    switch (p_event->type)
    {
        /*
         * --------------------------------------------------------------
         * RX byte complete
         * --------------------------------------------------------------
         */
        case NRF_DRV_UART_EVT_RX_DONE:
        {
            uint8_t byte =
                p_event->data.rxtx.p_data[0];


            /*
             * Single-producer / single-consumer FIFO.
             *
             * m_rx_head and m_rx_tail are monotonically increasing uint8_t
             * counters. Indexing into the actual storage uses modulo.
             */
            uint8_t used =
                (uint8_t)(
                    m_rx_head -
                    m_rx_tail
                );


            if (used <
                NRFCLAW_SERIAL_RX_BUFFER_SIZE)
            {
                m_rx_fifo[
                    m_rx_head %
                    NRFCLAW_SERIAL_RX_BUFFER_SIZE
                ] = byte;

                m_rx_head++;
            }

            /* Framing is event driven. IDLE mode restarts a low-frequency
             * app_timer after every byte; LINE and LENGTH can finish directly
             * in the UART callback. A full 64-byte FIFO is always emitted as a
             * frame so a continuous stream cannot silently overwrite data. */
            if (m_frame_waiting)
            {
                uint8_t avail = nrfclaw_serial_available();
                if (avail >= NRFCLAW_SERIAL_RX_BUFFER_SIZE)
                    serial_frame_complete(NRFCLAW_SERIAL_RX_BUFFER_SIZE);
                else if (m_frame_mode == NRFCLAW_SERIAL_FRAME_LINE && byte == (uint8_t)'\n')
                    serial_frame_complete(avail);
                else if (m_frame_mode == NRFCLAW_SERIAL_FRAME_LENGTH &&
                         m_frame_param > 0U && avail >= m_frame_param)
                    serial_frame_complete((uint8_t)m_frame_param);
                else if (m_frame_mode == NRFCLAW_SERIAL_FRAME_IDLE)
                {
                    uint16_t ms = m_frame_param ? m_frame_param : 20U;
                    (void)app_timer_stop(m_rx_idle_timer);
                    (void)app_timer_start(m_rx_idle_timer, APP_TIMER_TICKS(ms), NULL);
                }
            }


            /*
             * Publish an Event VM event even if the FIFO happened to be full.
             * arg0 carries the most recent byte and arg1 the FIFO occupancy.
             */
            /* Legacy byte events are useful for WAIT_EVENT/SERIAL_READ, but a
             * framed VM receiver would otherwise enqueue one event per byte and
             * waste energy/overflow the small event FIFO. While a framed read is
             * armed or waiting to be consumed, only SERIAL_FRAME is published. */
            if (!m_frame_waiting && !m_frame_ready)
            {
                nrfclaw_event_t evt =
                {
                    .type = NRFCLAW_EVT_SERIAL_RX,
                    .arg0 = byte,
                    .arg1 = nrfclaw_serial_available()
                };

                (void)nrfclaw_event_push_isr(&evt);
            }


            /*
             * Rearm one-byte asynchronous receive.
             */
            if (m_active && m_driver_initialized &&
                (!m_economy_mode || !m_frame_ready))
            {
                (void)nrf_drv_uart_rx(
                    &m_uart,
                    &m_rx_byte,
                    1U
                );
            }

            break;
        }


        /*
         * --------------------------------------------------------------
         * UART error
         * --------------------------------------------------------------
         */
        case NRF_DRV_UART_EVT_ERROR:
        {
            /*
             * Keep the capability alive after framing/noise/overrun errors.
             * The Nordic driver has already handled the peripheral event.
             */
            if (m_active && m_driver_initialized &&
                (!m_economy_mode || !m_frame_ready))
            {
                (void)nrf_drv_uart_rx(
                    &m_uart,
                    &m_rx_byte,
                    1U
                );
            }

            break;
        }


        case NRF_DRV_UART_EVT_TX_DONE:
        {
            m_tx_busy =
                false;

            nrfclaw_event_t evt =
            {
                .type = NRFCLAW_EVT_SERIAL_TX_DONE,
                .arg0 = p_event->data.rxtx.bytes,
                .arg1 = 0U
            };

            (void)nrfclaw_event_push_isr(
                &evt
            );

            break;
        }

        default:
            break;
    }
}


/*
 * --------------------------------------------------------------------------
 * Initialization
 * --------------------------------------------------------------------------
 */
void nrfclaw_serial_init(void)
{
    m_rx_head =
        0U;

    m_rx_tail =
        0U;

    m_rx_byte =
        0U;

    m_tx_busy =
        false;

    m_frame_waiting = false;
    m_frame_ready = false;
    m_frame_len = 0U;
    m_frame_mode = NRFCLAW_SERIAL_FRAME_IDLE;
    m_frame_param = 20U;
    APP_ERROR_CHECK(app_timer_create(&m_rx_idle_timer, APP_TIMER_MODE_SINGLE_SHOT, serial_idle_timer_handler));


    /*
     * SERIAL is intentionally inactive at boot.
     *
     * These values define only the default configuration that will be used
     * if a program later requests SERIAL with the default mapping.
     */
    m_active =
        false;
    m_economy_mode = false;
    m_driver_initialized = false;
    m_wake_pending = false;
    m_sleep_pending = false;
    m_wake_configured = false;

    m_tx_pin =
        P_SERIAL_TX_DEFAULT;

    m_rx_pin =
        P_SERIAL_RX_DEFAULT;

    m_baud =
        NRFCLAW_SERIAL_DEFAULT_BAUD;
}


/*
 * --------------------------------------------------------------------------
 * Enable / configure SERIAL
 * --------------------------------------------------------------------------
 */
bool nrfclaw_serial_enable(
    uint8_t tx_pin,
    uint8_t rx_pin,
    uint32_t baud
)
{
    /*
     * nRF52832 GPIO range.
     */
    if (tx_pin > 31U ||
        rx_pin > 31U ||
        tx_pin == rx_pin)
    {
        return false;
    }


    nrf_uart_baudrate_t sdk_baud =
        baud_to_sdk(
            baud
        );


    if ((uint32_t)sdk_baud == 0U)
    {
        return false;
    }


    /*
     * Allow runtime remapping.
     */
    nrfclaw_serial_disable();


    nrf_drv_uart_config_t config =
        NRF_DRV_UART_DEFAULT_CONFIG;


    config.pseltxd =
        tx_pin;

    config.pselrxd =
        rx_pin;

    config.pselcts =
        NRF_UART_PSEL_DISCONNECTED;

    config.pselrts =
        NRF_UART_PSEL_DISCONNECTED;


    /*
     * Safe default requested for Stage 8.1:
     *
     *     57600 8N1
     *
     * Baud is configurable, but framing remains 8 data bits, no parity,
     * 1 stop bit, without HW flow control.
     */
    config.baudrate =
        sdk_baud;

    config.hwfc =
        NRF_UART_HWFC_DISABLED;

    config.parity =
        NRF_UART_PARITY_EXCLUDED;


    /*
     * Match the low-priority interrupt policy used by the Event VM drivers.
     */
    config.interrupt_priority =
        7U;


    ret_code_t err =
        nrf_drv_uart_init(
            &m_uart,
            &config,
            uart_event_handler
        );


    if (err != NRF_SUCCESS)
    {
        release_pin(
            tx_pin
        );

        release_pin(
            rx_pin
        );

        return false;
    }


    m_tx_pin =
        tx_pin;

    m_rx_pin =
        rx_pin;

    m_baud =
        baud;

    m_rx_head =
        0U;

    m_rx_tail =
        0U;

    m_active =
        true;
    m_economy_mode = false;
    m_driver_initialized = true;
    m_wake_pending = false;
    m_sleep_pending = false;
    /* UART idle is HIGH; an internal pull-up also prevents a disconnected RX
     * from turning line noise into an interrupt storm. */
    nrf_gpio_cfg_input(m_rx_pin, NRF_GPIO_PIN_PULLUP);


    /*
     * Start interrupt-driven receive.
     */
    err =
        nrf_drv_uart_rx(
            &m_uart,
            &m_rx_byte,
            1U
        );


    if (err != NRF_SUCCESS)
    {
        nrf_drv_uart_uninit(
            &m_uart
        );
        m_driver_initialized = false;

        release_pin(
            m_tx_pin
        );

        release_pin(
            m_rx_pin
        );

        m_active =
            false;

        return false;
    }


    return true;
}



bool nrfclaw_serial_enable_economy(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud)
{
    if (tx_pin > 31U || rx_pin > 31U || tx_pin == rx_pin) return false;
    if ((uint32_t)baud_to_sdk(baud) == 0U) return false;

    nrfclaw_serial_disable();

    m_tx_pin = tx_pin;
    m_rx_pin = rx_pin;
    m_baud = baud;
    m_rx_head = 0U;
    m_rx_tail = 0U;
    m_tx_busy = false;
    m_active = true;
    m_economy_mode = true;
    m_driver_initialized = false;
    m_wake_pending = false;
    m_sleep_pending = false;

    /* TX is not needed while waiting. RX is kept HIGH and will gain SENSE when
     * SERIAL_RX_BUF arms the next frame. No UARTE/HFCLK request exists here. */
    release_pin(m_tx_pin);
    nrf_gpio_cfg_input(m_rx_pin, NRF_GPIO_PIN_PULLUP);
    return true;
}

bool nrfclaw_serial_economy_mode(void)
{
    return m_active && m_economy_mode;
}

void nrfclaw_serial_process(void)
{
    if (!m_active || !m_economy_mode) return;

    if (m_sleep_pending) {
        m_sleep_pending = false;
        m_wake_pending = false;
        serial_driver_stop();
        if (m_frame_waiting) (void)serial_wake_arm();
    }

    if (m_wake_pending && m_frame_waiting && !m_frame_ready) {
        m_wake_pending = false;
        /* The GPIO edge belongs to a sacrificial wake byte. Wait until that
         * character has certainly finished before attaching UARTE so preamble
         * fragments cannot leak into the forwarded VM payload. 8N1 = 10 bits. */
        uint32_t wake_char_us = (10000000UL + m_baud - 1UL) / m_baud;
        wake_char_us += 100UL;
        nrf_delay_us(wake_char_us);
        if (!serial_driver_start_rx()) {
            /* Recover to low-power wake state instead of spinning active. */
            serial_driver_stop();
            (void)serial_wake_arm();
        }
    }
}

/*
 * --------------------------------------------------------------------------
 * Disable SERIAL
 * --------------------------------------------------------------------------
 */
void nrfclaw_serial_disable(void)
{
    nrfclaw_serial_frame_cancel();
    if (!m_active)
    {
        return;
    }


    /* Stop UARTE only when it is actually initialized; economy mode spends
     * most of its lifetime with no peripheral instance active. */
    serial_wake_release();
    serial_driver_stop();


    /*
     * Return pins to the board's generic low-power GPIO policy.
     */
    release_pin(
        m_tx_pin
    );

    release_pin(
        m_rx_pin
    );


    m_rx_head =
        0U;

    m_rx_tail =
        0U;

    m_tx_busy =
        false;

    m_active =
        false;
    m_economy_mode = false;
    m_wake_pending = false;
    m_sleep_pending = false;
}


/*
 * --------------------------------------------------------------------------
 * Status
 * --------------------------------------------------------------------------
 */
bool nrfclaw_serial_active(void)
{
    return
        m_active;
}


uint8_t nrfclaw_serial_tx_pin(void)
{
    return
        m_tx_pin;
}


uint8_t nrfclaw_serial_rx_pin(void)
{
    return
        m_rx_pin;
}


uint32_t nrfclaw_serial_baud(void)
{
    return
        m_baud;
}


bool nrfclaw_serial_owns_pin(
    uint8_t pin
)
{
    return
        m_active &&
        (
            pin == m_tx_pin ||
            pin == m_rx_pin
        );
}


/*
 * --------------------------------------------------------------------------
 * TX
 * --------------------------------------------------------------------------
 *
 * nrf_drv_uart_tx() is asynchronous when a handler is supplied.
 *
 * The caller must keep the source buffer valid until TX_DONE. VM bytecode
 * resides in persistent RAM/Flash-backed program storage for the lifetime
 * of the operation, so SERIAL_WRITE can safely point into VM program data.
 */
bool nrfclaw_serial_write(
    uint8_t const * data,
    uint16_t len
)
{
    if (!m_active ||
        data == NULL ||
        len == 0U ||
        len > sizeof(m_tx_buffer))
    {
        return false;
    }


    if (!m_driver_initialized) {
        if (!serial_driver_start_rx()) return false;
    }

    /*
     * Only one asynchronous TX may be in flight.
     */
    if (m_tx_busy)
    {
        return false;
    }


    memcpy(
        m_tx_buffer,
        data,
        len
    );


    m_tx_busy =
        true;


    ret_code_t err =
        nrf_drv_uart_tx(
            &m_uart,
            m_tx_buffer,
            len
        );


    if (err != NRF_SUCCESS)
    {
        m_tx_busy =
            false;

        return false;
    }


    /*
     * Completion is reported asynchronously through
     * NRFCLAW_EVT_SERIAL_TX_DONE.
     */
    return true;
}


/*
 * --------------------------------------------------------------------------
 * RX FIFO
 * --------------------------------------------------------------------------
 */
uint8_t nrfclaw_serial_available(void)
{
    return
        (uint8_t)(
            m_rx_head -
            m_rx_tail
        );
}


bool nrfclaw_serial_read_byte(
    uint8_t * value
)
{
    if (value == NULL)
    {
        return false;
    }


    if (m_rx_head ==
        m_rx_tail)
    {
        return false;
    }


    *value =
        m_rx_fifo[
            m_rx_tail %
            NRFCLAW_SERIAL_RX_BUFFER_SIZE
        ];


    m_rx_tail++;


    return true;
}


/* --------------------------------------------------------------------------
 * R3.8.18c framed RX API
 * -------------------------------------------------------------------------- */
bool nrfclaw_serial_frame_arm(nrfclaw_serial_frame_mode_t mode, uint16_t param)
{
    if (!m_active || m_frame_waiting || m_frame_ready) return false;
    if (mode > NRFCLAW_SERIAL_FRAME_LENGTH) return false;
    if (mode == NRFCLAW_SERIAL_FRAME_LENGTH && (param == 0U || param > NRFCLAW_SERIAL_RX_BUFFER_SIZE)) return false;
    if (mode == NRFCLAW_SERIAL_FRAME_IDLE && param == 0U) param = 20U;

    m_frame_mode = mode;
    m_frame_param = param;
    m_frame_waiting = true;

    /* Economy policy: do not initialize UARTE here. Arm low-accuracy GPIO
     * wake and let the user's preamble wake us. Continuous mode is unchanged. */
    if (m_economy_mode && !m_driver_initialized) {
        if (!serial_wake_arm()) {
            m_frame_waiting = false;
            return false;
        }
    }

    /* Bytes may already be queued before the VM arms the next frame. */
    uint8_t avail = nrfclaw_serial_available();
    if (avail != 0U) {
        if (mode == NRFCLAW_SERIAL_FRAME_LENGTH && avail >= param)
            serial_frame_complete((uint8_t)param);
        else if (mode == NRFCLAW_SERIAL_FRAME_LINE) {
            uint8_t n = 0U;
            uint8_t tail = m_rx_tail;
            while (n < avail) {
                uint8_t b = m_rx_fifo[(uint8_t)(tail + n) % NRFCLAW_SERIAL_RX_BUFFER_SIZE];
                n++;
                if (b == (uint8_t)'\n') { serial_frame_complete(n); break; }
            }
        } else if (mode == NRFCLAW_SERIAL_FRAME_IDLE) {
            (void)app_timer_start(m_rx_idle_timer, APP_TIMER_TICKS(param), NULL);
        }
    }
    return true;
}

bool nrfclaw_serial_frame_take(uint8_t *dst, uint8_t max_len, uint8_t *out_len)
{
    if (!dst || !out_len || !m_frame_ready || m_frame_len == 0U || m_frame_len > max_len) return false;
    uint8_t n = m_frame_len;
    for (uint8_t i = 0U; i < n; ++i) {
        if (!nrfclaw_serial_read_byte(&dst[i])) return false;
    }
    m_frame_len = 0U;
    m_frame_ready = false;
    *out_len = n;
    return true;
}

void nrfclaw_serial_frame_cancel(void)
{
    (void)app_timer_stop(m_rx_idle_timer);
    m_frame_waiting = false;
    m_frame_ready = false;
    m_frame_len = 0U;
    if (m_economy_mode) {
        m_wake_pending = false;
        m_sleep_pending = false;
        serial_wake_release();
        serial_driver_stop();
    }
}
