#ifndef NRFCLAW_OPCODES_H
#define NRFCLAW_OPCODES_H

#include <stdint.h>

/*
 * nRFClaw bytecode ABI.
 *
 * Existing opcode values are frozen for backward compatibility.
 * Stage 9.0 fills previously unused values around the Stage-2 expression core.
 */
typedef enum
{
    OP_END              = 0x00,

    /* Stage 9.0 arithmetic / register expressions. */
    OP_MOV              = 0x01, /* dst:u8, src:u8 */
    OP_ADD              = 0x02, /* dst:u8, a:u8, b:u8 */
    OP_SUB              = 0x03, /* dst:u8, a:u8, b:u8 */
    OP_MUL              = 0x04, /* dst:u8, a:u8, b:u8 */
    OP_DIV              = 0x05, /* signed int32; VM_ERROR on /0 */
    OP_MOD              = 0x06, /* signed int32; VM_ERROR on %0 */
    OP_NEG              = 0x07, /* dst:u8, src:u8 */

    /* Stage-2 legacy expression/control opcodes. */
    OP_MOVI             = 0x08, /* dst:u8, imm:u32le */
    OP_CMP_GE           = 0x09, /* signed: dst,a,b */
    OP_JNZ              = 0x0A, /* reg:u8, rel:i16le */
    OP_JMP              = 0x0B, /* rel:i16le */

    /* Stage 9.0 comparison completion. Results are exactly 0 or 1. */
    OP_CMP_EQ           = 0x0C, /* bitwise equality: dst,a,b */
    OP_CMP_NE           = 0x0D, /* bitwise inequality: dst,a,b */
    OP_CMP_LT           = 0x0E, /* signed int32: dst,a,b */
    OP_CMP_LE           = 0x0F, /* signed int32: dst,a,b */
    OP_CMP_GT           = 0x10, /* signed int32: dst,a,b */

    OP_WAIT_S           = 0x11,
    OP_WAIT_EVENT       = 0x12,
    OP_JZ              = 0x13, /* reg:u8, rel:i16le */
    OP_WAIT_HALL        = 0x22,
    OP_DEBUG_I32        = 0x23,
    OP_BAT_READ         = 0x30,
    OP_DEBUG_BAT        = 0x31,
    OP_NOTIFY_U32       = 0x40,
    OP_NOTIFY_STR       = 0x41,
    OP_STATE_SET        = 0x42,
    OP_STATE_GET        = 0x43,
    OP_PERSIST_SAVE     = 0x44,
    OP_PERSIST_LOAD     = 0x45,
    OP_GPIO_WRITE_IMM   = 0x50,
    OP_GPIO_WRITE_REG   = 0x51,
    OP_GPIO_READ        = 0x52,
    OP_ACCEL_READ       = 0x53,
    OP_ACCEL_MOTION     = 0x54,
    OP_CAP_AVAILABLE    = 0x55,
    OP_APP_GET_U32      = 0x56,
    OP_APP_SEND_U32     = 0x57,
    OP_SERIAL_CONFIG    = 0x58,
    OP_SERIAL_DISABLE   = 0x59,
    OP_SERIAL_WRITE     = 0x5A,
    OP_SERIAL_READ      = 0x5B,
    OP_ACCEL_CONFIG     = 0x5C, /* mode:u8 odr:u16 fs:u8 th:u16 dur_ms:u16 flags:u8 */
    OP_DS18_READ        = 0x5D, /* dst register, milli-degC; async */
    OP_VIB_MODEL_SET    = 0x5E, /* EXP: 8 x u16le model fields */
    OP_VIB_SCORE        = 0x5F, /* EXP: dst register, Q8.8 score */
    OP_LORA_SEND_BAT    = 0x60,
    OP_LORA_SEND_BYTES  = 0x61,
    /* R3.8.16a compositional runtime primitives. */
    OP_LORA_CONFIG      = 0x62, /* freq:u32 power:i8 sf:u8 bw:u16 cr:u8 */
    OP_FORMAT_REG       = 0x63, /* prefix_len:u8 prefix[] reg:u8 format:u8 -> VM buffer */
    OP_LORA_SEND_BUF    = 0x64, /* send current VM buffer */
    OP_LORA_RX_START    = 0x65, /* flags:u8 bit0=continuous-until-packet; result -> VM buffer */
    OP_SERIAL_WRITE_BUF = 0x66, /* write current VM buffer */
    OP_BLE_ADV_BUF      = 0x67, /* replace advertiser local-name/data from VM buffer */
    OP_APP_EVENT_SEND   = 0x68, /* capability:u8 operation:u8 reg:u8; one-slot mailbox */
    OP_DEBUG_BUFFER     = 0x69, /* RTT dump current VM buffer: len + HEX + printable ASCII */
    OP_BLE_APP_ROLE     = 0x70,
    OP_BLE_ADV_CONFIG   = 0x71,
    OP_BLE_ADV_START    = 0x72,
    OP_BLE_ADV_STOP     = 0x73,
    OP_BLE_CONNECT      = 0x74,
    OP_BLE_DISCONNECT   = 0x75,
    OP_BLE_ADV_NAME     = 0x76, /* len:u8 UTF-8 local name */
    OP_TRACKING_SET_KEY = 0x77,
    OP_TRACKING_CONFIG  = 0x78,
    OP_TRACKING_START   = 0x79,
    OP_TRACKING_STOP    = 0x7A,
    OP_TRACKING_STATUS  = 0x7B,
    OP_TRACKING_ROTATION_SET = 0x7C,
    OP_VIB_AUTO_START   = 0x80, /* flags:u8 bit0=relearn */
    OP_VIB_AUTO_STOP    = 0x81,
    OP_HALL_CONFIG      = 0x82, /* mode:u8 channel:u8 pullup:u8 count:u8 events:u8 */
    OP_VIB_AUTO_CONFIG_TIME = 0x83, /* learning_time_s:u32 LE, arming_delay_s:u32 LE */
    OP_SYSTEM_MIN_POWER = 0x84, /* no payload; preserve P0.21 programming wake */
    OP_SERIAL_RX_BUF     = 0x85, /* mode:u8 param:u16; async framed UART RX -> VM buffer */
    OP_APP_SEND_BUF      = 0x86, /* raw Application/NDP notify from VM buffer, max 20 bytes */
    OP_SERIAL_CONFIG_ECO = 0x87, /* tx:u8 rx:u8 baud:u32; UART off while idle, GPIO RX wake */
    OP_FORMAT_2REG      = 0x88, /* p1_len:u8 p1[] r1:u8 fmt1:u8 p2_len:u8 p2[] r2:u8 fmt2:u8 */
    OP_PARSE_BUF_I32    = 0x89, /* dst:u8; parse first signed integer token from VM buffer */
    OP_PARSE_BUF_FIXED  = 0x8A, /* dst:u8 decimals:u8; parse first signed decimal token, scaled by 10^decimals */
    OP_FORMAT_REG_FIXED = 0x8B, /* prefix_len:u8 prefix[] reg:u8 decimals:u8 suffix_len:u8 suffix[] */
    OP_BUFFER_PREPEND    = 0x8C, /* prefix_len:u8 prefix[]; prepend literal to current VM buffer */
    OP_BUFFER_SET        = 0x8D  /* len:u8 bytes[]; replace current VM buffer with literal */,
    OP_NINALINK_TELEMETRY = 0x8E /* period_s:u16 window_ms:u16 attempts:u8 backoff_ms:u16 */,
    OP_NINALINK_TELEMETRY_SYNTH = 0x8F /* period:u16 window:u16 attempts:u8 backoff:u16 temp_mC:i32 */,
    OP_LORA_LAST_RSSI = 0x90, /* dst:u8; last valid LoRa packet RSSI, signed whole dBm */
    OP_HA_ROLE_CONFIG = 0x91, /* role:u8; legacy/general persist + apply HA transport role */
    OP_LORA_PROFILE_PERSIST = 0x92, /* mask:u8 + frozen 9-byte RF fields; preserve unmasked fields */
    OP_HA_NINALINK_NODE_CONFIG = 0x93, /* period_s:u16; persist NINALINK_NODE role + report period atomically */
    OP_SEMANTIC_PUBLISH = 0x94, /* semantic_id:u16 channel:u8 reg:u8; retained VM semantic state */
    OP_PERSIST_LOAD_DEFAULT = 0x95 /* key:u8 reg:u8 default:u32le; missing key becomes RAM default */
} nrfclaw_opcode_t;

#define NRFCLAW_VM_REGISTER_COUNT 8U

#endif /* NRFCLAW_OPCODES_H */
