#ifndef NRFCLAW_EVENT_H
#define NRFCLAW_EVENT_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NRFCLAW_EVT_NONE = 0,
    NRFCLAW_EVT_BOOT,
    NRFCLAW_EVT_BUTTON,
    NRFCLAW_EVT_HALL,
    NRFCLAW_EVT_RTC,
    NRFCLAW_EVT_BLE_CONNECTED,
    NRFCLAW_EVT_BLE_DISCONNECTED,
    NRFCLAW_EVT_NUS_RX,
    NRFCLAW_EVT_LORA_DIO1,
    NRFCLAW_EVT_LORA_CLEANUP,
    NRFCLAW_EVT_LORA_TX_DONE,
    NRFCLAW_EVT_LORA_RX_DONE,
    NRFCLAW_EVT_LORA_TIMEOUT,
    NRFCLAW_EVT_VM_WAKE,
    NRFCLAW_EVT_BATTERY_STEP,
    NRFCLAW_EVT_BATTERY_DONE,
    NRFCLAW_EVT_ACCEL_MOTION,
    NRFCLAW_EVT_ACCEL_INT2,
    NRFCLAW_EVT_APP_COMMAND,
    NRFCLAW_EVT_APP_CONNECTED,
    NRFCLAW_EVT_APP_DISCONNECTED,

    /* Stage 8.1 SERIAL events. */
    NRFCLAW_EVT_SERIAL_RX,
    NRFCLAW_EVT_SERIAL_TX_DONE,

    /* Pack 02-r2 sensor events. */
    NRFCLAW_EVT_ACCEL_TAP,
    NRFCLAW_EVT_ACCEL_FALL,
    NRFCLAW_EVT_ACCEL_WALK,
    NRFCLAW_EVT_ACCEL_VIBRATION_READY,
    NRFCLAW_EVT_DS18B20_STEP,
    NRFCLAW_EVT_DS18B20_DONE,

    /* r3.8.11 internal FIFO watchdog event. Appended to preserve existing event IDs. */
    NRFCLAW_EVT_ACCEL_VIBRATION_POLL,

    /* R3.8.18c framed SERIAL source completion; appended to preserve event IDs. */
    NRFCLAW_EVT_SERIAL_FRAME,

    /* B7.6f2i1 logical temperature completion; appended to preserve event IDs. */
    NRFCLAW_EVT_TEMPERATURE_DONE,

    /* B7.6f2l6a internal deferred INT1 qualification. Never forwarded to VM/user consumers. */
    NRFCLAW_EVT_ACCEL_INT1_RAW

} nrfclaw_event_type_t;

typedef struct {
    nrfclaw_event_type_t type;
    uint32_t arg0;
    uint32_t arg1;
} nrfclaw_event_t;

void nrfclaw_event_init(void);
bool nrfclaw_event_push_isr(nrfclaw_event_t const *evt);
bool nrfclaw_event_push(nrfclaw_event_t const *evt);
bool nrfclaw_event_pop(nrfclaw_event_t *evt);

#endif
