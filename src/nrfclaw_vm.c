#include "nrfclaw_vm.h"
#include "nrfclaw_ninalink_telemetry.h"
#include "nrfclaw_opcodes.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_battery.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_flash.h"
#include "nrfclaw_auth.h"
#include "nrfclaw_scheduler.h"
#include "nrfclaw_state.h"
#include "nrfclaw_native.h"
#include "nrfclaw_serial.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_system_power.h"
#include "nrfclaw_ha_role.h"
#include "nrfclaw_ha_ninalink_node.h"
#include "nrfclaw_capability.h"
#include "nrfclaw_vm_semantic_state.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Bytecode opcode definitions are public from Stage 9.0 onward.
 * See include/nrfclaw_opcodes.h.
 * ------------------------------------------------------------------------- */

#define VM_REG_COUNT              NRFCLAW_VM_REGISTER_COUNT
#define VM_MAX_STEPS_PER_TICK     16U

/* Stage-2 regression programs remain available but are no longer
 * automatically executed by main.c in Stage 3. */
static const uint8_t m_test2_program[] = {
    OP_WAIT_S, 0x01, 0x00, 0x00, 0x00,
    OP_WAIT_S, 0x02, 0x00, 0x00, 0x00,
    OP_WAIT_S, 0x03, 0x00, 0x00, 0x00,
    OP_END
};

static const uint8_t m_test3_program[] = {
    OP_WAIT_HALL, 0x00,
    OP_DEBUG_I32, 0x00,
    OP_END
};

static const uint8_t m_test4_program[] = {
    OP_WAIT_S, 0x05, 0x00, 0x00, 0x00,
    OP_BAT_READ, 0x00,
    OP_DEBUG_BAT, 0x00,
    OP_LORA_SEND_BAT, 0x00,
    OP_END
};

static const uint8_t m_test5_program[] = {
    0xFF,
    OP_END
};

/* -------------------------------------------------------------------------
 * Runtime
 * ------------------------------------------------------------------------- */
static nrfclaw_vm_state_t m_state;
static const uint8_t *m_code;
static uint16_t m_code_len;
static uint16_t m_pc;
static uint32_t m_reg[VM_REG_COUNT];
static uint8_t m_pending_battery_reg;
static uint8_t m_pending_ds18_reg;
static uint8_t m_pending_hall_reg;
static uint8_t m_pending_event_type;
static uint8_t m_pending_event_reg0;
static uint8_t m_pending_event_reg1;
static nrfclaw_vm_notify_handler_t m_notify_handler;

/* B7.6f2l3 semantic accumulator ownership. Persistent user-state load/save
 * binds one state key to a VM register; SEMANTIC_PUBLISH snapshots that
 * binding. This lets HA reset the retained accumulator generically instead of
 * hard-coding the current compiler's state[15]/R1 choice. */
static bool m_reg_state_key_valid[VM_REG_COUNT];
static uint8_t m_reg_state_key[VM_REG_COUNT];
static bool m_semantic_accumulator_bound;
static uint8_t m_semantic_accumulator_reg;
static uint8_t m_semantic_accumulator_key;
static uint16_t m_semantic_accumulator_capability;
static uint8_t m_semantic_accumulator_channel;
static bool m_semantic_reset_restart_pending;

/* R3.8.16a generic VM scratch buffer. It decouples producers (formatters,
 * LoRa RX) from consumers (LoRa TX, UART TX, BLE advertiser). */
static uint8_t m_buffer[NRFCLAW_VM_BUFFER_SIZE];
static uint8_t m_buffer_len;

/* One-slot Application/HA event mailbox. A disconnected HA must not turn a
 * physical sensor event into VM_ERROR. The frame is retried when possible. */
static bool m_app_event_pending;
static nrfclaw_app_frame_t m_app_event_frame;
static uint8_t m_app_event_sequence;

/* Stage 3 RAM program slot. */
static uint8_t  m_program[NRFCLAW_VM_MAX_PROGRAM_SIZE];
static uint16_t m_program_len;
static bool     m_program_loaded;

/* Sequential upload state. */
static bool     m_upload_active;
static uint16_t m_upload_expected_len;
static uint16_t m_upload_expected_crc;
static uint16_t m_upload_written;
static uint8_t  m_upload_auth_tag[NRFCLAW_AUTH_TAG_SIZE];
static uint8_t  m_upload_auth_written;
static nrfclaw_schedule_t m_upload_schedule;

/* ------------------------------------------------------------------------- */
static uint32_t read_u32_le(uint8_t const *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t read_i16_le(uint8_t const *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool jump_relative(int16_t rel)
{
    int32_t target = (int32_t)m_pc + (int32_t)rel;
    if (target < 0 || target >= (int32_t)m_code_len)
        return false;
    m_pc = (uint16_t)target;
    return true;
}

static bool vm_format_reg_value(uint8_t reg, uint8_t format, char *value, size_t value_size, int *out_n)
{
    int n = 0;
    if (reg >= VM_REG_COUNT || !value || value_size == 0U || !out_n) return false;

    switch (format) {
        case 0U:
            n = snprintf(value, value_size, "%lu", (unsigned long)m_reg[reg]);
            break;
        case 1U:
            n = snprintf(value, value_size, "%ld", (long)(int32_t)m_reg[reg]);
            break;
        case 2U: {
            uint32_t cv = m_reg[reg];
            n = snprintf(value, value_size, "%lu.%02luV",
                         (unsigned long)(cv / 100U), (unsigned long)(cv % 100U));
            break;
        }
        case 3U: {
            int32_t mc = (int32_t)m_reg[reg];
            int32_t a = mc < 0 ? -mc : mc;
            n = snprintf(value, value_size, "%s%ld.%03ldC",
                         mc < 0 ? "-" : "", (long)(a / 1000), (long)(a % 1000));
            break;
        }
        case 4U:
            n = snprintf(value, value_size, "%02lu", (unsigned long)m_reg[reg]);
            break;
        default:
            return false;
    }
    if (n < 0 || (size_t)n >= value_size) return false;
    *out_n = n;
    return true;
}

static bool vm_parse_first_signed_fixed(uint8_t decimals, int32_t *out)
{
    if (!out || decimals > 6U || m_buffer_len == 0U) return false;
    uint32_t scale = 1U;
    for (uint8_t i=0U;i<decimals;i++) scale *= 10U;
    uint8_t i=0U;
    while (i < m_buffer_len) {
        uint8_t c=m_buffer[i];
        if ((c>='0'&&c<='9') || ((c=='-'||c=='+') && i+1U<m_buffer_len && m_buffer[i+1U]>='0'&&m_buffer[i+1U]<='9')) break;
        i++;
    }
    if (i>=m_buffer_len) return false;
    bool neg=false;
    if (m_buffer[i]=='-' || m_buffer[i]=='+') { neg=m_buffer[i]=='-'; i++; }
    int64_t whole=0; bool any=false;
    while (i<m_buffer_len && m_buffer[i]>='0'&&m_buffer[i]<='9') { any=true; whole=whole*10+(m_buffer[i]-'0'); if (whole>2147483648LL) return false; i++; }
    if (!any) return false;
    uint32_t frac=0U, fd=0U;
    if (i<m_buffer_len && (m_buffer[i]=='.'||m_buffer[i]==',')) {
        i++;
        while (i<m_buffer_len && m_buffer[i]>='0'&&m_buffer[i]<='9') {
            if (fd<decimals) { frac=frac*10U+(uint32_t)(m_buffer[i]-'0'); fd++; }
            i++;
        }
    }
    while (fd<decimals) { frac*=10U; fd++; }
    int64_t v=whole*(int64_t)scale+(int64_t)frac;
    if (neg) v=-v;
    if (v < INT32_MIN || v > INT32_MAX) return false;
    *out=(int32_t)v; return true;
}

static bool vm_buffer_format_fixed(uint8_t const *prefix,uint8_t plen,uint8_t reg,uint8_t decimals,uint8_t const *suffix,uint8_t slen)
{
    if (reg>=VM_REG_COUNT || decimals>6U) return false;
    uint32_t scale=1U; for(uint8_t i=0U;i<decimals;i++) scale*=10U;
    int32_t v=(int32_t)m_reg[reg]; int64_t a=v<0?-(int64_t)v:(int64_t)v;
    char num[32]; int n;
    if (decimals==0U) n=snprintf(num,sizeof(num),"%s%ld",v<0?"-":"",(long)a);
    else n=snprintf(num,sizeof(num),"%s%ld.%0*ld",v<0?"-":"",(long)(a/scale),(int)decimals,(long)(a%scale));
    if(n<0 || (size_t)n>=sizeof(num)) return false;
    uint16_t total=(uint16_t)plen+(uint16_t)n+(uint16_t)slen;
    if(total>NRFCLAW_VM_BUFFER_SIZE) return false;
    uint8_t pos=0U; if(plen){memcpy(&m_buffer[pos],prefix,plen);pos+=plen;} memcpy(&m_buffer[pos],num,(size_t)n);pos+=(uint8_t)n; if(slen){memcpy(&m_buffer[pos],suffix,slen);pos+=slen;} m_buffer_len=pos; return true;
}

static bool vm_buffer_format_reg(uint8_t const *prefix, uint8_t prefix_len,
                                 uint8_t reg, uint8_t format)
{
    char value[32];
    int n = 0;
    if (prefix_len > NRFCLAW_VM_BUFFER_SIZE ||
        !vm_format_reg_value(reg, format, value, sizeof(value), &n)) return false;
    if ((uint16_t)prefix_len + (uint16_t)n > NRFCLAW_VM_BUFFER_SIZE) return false;
    if (prefix_len) memcpy(m_buffer, prefix, prefix_len);
    memcpy(&m_buffer[prefix_len], value, (size_t)n);
    m_buffer_len = (uint8_t)(prefix_len + (uint8_t)n);
    return true;
}

static bool vm_buffer_format_2reg(uint8_t const *p1, uint8_t p1_len, uint8_t r1, uint8_t f1,
                                  uint8_t const *p2, uint8_t p2_len, uint8_t r2, uint8_t f2)
{
    char v1[32], v2[32];
    int n1=0, n2=0;
    if (!vm_format_reg_value(r1,f1,v1,sizeof(v1),&n1) ||
        !vm_format_reg_value(r2,f2,v2,sizeof(v2),&n2)) return false;
    uint16_t total=(uint16_t)p1_len+(uint16_t)n1+(uint16_t)p2_len+(uint16_t)n2;
    if (total > NRFCLAW_VM_BUFFER_SIZE) return false;
    uint8_t pos=0U;
    if (p1_len) { memcpy(&m_buffer[pos],p1,p1_len); pos=(uint8_t)(pos+p1_len); }
    memcpy(&m_buffer[pos],v1,(size_t)n1); pos=(uint8_t)(pos+(uint8_t)n1);
    if (p2_len) { memcpy(&m_buffer[pos],p2,p2_len); pos=(uint8_t)(pos+p2_len); }
    memcpy(&m_buffer[pos],v2,(size_t)n2); pos=(uint8_t)(pos+(uint8_t)n2);
    m_buffer_len=pos;
    return true;
}

static void app_event_try_send(void)
{
    if (!m_app_event_pending) return;
    nrfclaw_ble_app_status_t st = nrfclaw_ble_app_send(&m_app_event_frame);
    if (st == NRFCLAW_BLE_APP_OK)
        m_app_event_pending = false;
}

static uint16_t crc16_ccitt(uint8_t const *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;

        for (uint8_t bit = 0; bit < 8U; bit++) {
            if (crc & 0x8000U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc <<= 1;
        }
    }

    return crc;
}

static void vm_error(char const *msg)
{
    (void)msg;
    m_state = NRFCLAW_VM_ERROR;
}


/* Validate the complete bytecode before it can become executable.
 * The validator never executes opcodes or touches hardware. */
static bool validate_program(uint8_t const *code, uint16_t len)
{
    uint16_t pc = 0;

    /*
     * Compact boundary bitmap + jump-target list.
     * Validation scratch space scales with NRFCLAW_VM_MAX_PROGRAM_SIZE (1024 bytes).
     */
    uint8_t boundary[(NRFCLAW_VM_MAX_PROGRAM_SIZE + 7U) / 8U];
    int32_t jump_target[(NRFCLAW_VM_MAX_PROGRAM_SIZE / 3U) + 1U];
    uint16_t jump_count = 0U;

    if (!code || len == 0U || len > NRFCLAW_VM_MAX_PROGRAM_SIZE)
        return false;

    memset(boundary, 0, sizeof(boundary));
    memset(jump_target, 0, sizeof(jump_target));

    while (pc < len) {
        uint16_t instruction_pc = pc;
        boundary[instruction_pc >> 3] |=
            (uint8_t)(1U << (instruction_pc & 7U));

        uint8_t op = code[pc++];

        switch (op) {
            case OP_END:
                if (pc != len)
                    return false;

                for (uint16_t i = 0U; i < jump_count; i++) {
                    int32_t target = jump_target[i];

                    if (target < 0 || target >= (int32_t)len)
                        return false;

                    uint16_t target_u16 = (uint16_t)target;

                    if ((boundary[target_u16 >> 3] &
                         (uint8_t)(1U << (target_u16 & 7U))) == 0U)
                        return false;
                }

                return true;

            /* Stage 9.0: one destination + one source register. */
            case OP_MOV:
            case OP_NEG:
                if ((uint16_t)(pc + 2U) > len ||
                    code[pc] >= VM_REG_COUNT ||
                    code[pc + 1U] >= VM_REG_COUNT)
                    return false;
                pc += 2U;
                break;

            /* Stage 9.0: dst, lhs, rhs. */
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD:
            case OP_CMP_EQ:
            case OP_CMP_NE:
            case OP_CMP_LT:
            case OP_CMP_LE:
            case OP_CMP_GT:
                if ((uint16_t)(pc + 3U) > len ||
                    code[pc] >= VM_REG_COUNT ||
                    code[pc + 1U] >= VM_REG_COUNT ||
                    code[pc + 2U] >= VM_REG_COUNT)
                    return false;
                pc += 3U;
                break;

            case OP_MOVI:
                if ((uint16_t)(pc + 5U) > len || code[pc] >= VM_REG_COUNT)
                    return false;
                pc += 5U;
                break;

            case OP_CMP_GE:
                if ((uint16_t)(pc + 3U) > len)
                    return false;
                if (code[pc] >= VM_REG_COUNT ||
                    code[pc+1U] >= VM_REG_COUNT ||
                    code[pc+2U] >= VM_REG_COUNT)
                    return false;
                pc += 3U;
                break;

            case OP_JNZ:
            case OP_JZ: {
                if ((uint16_t)(pc + 3U) > len || code[pc] >= VM_REG_COUNT)
                    return false;

                int16_t rel = read_i16_le(&code[pc + 1U]);
                uint16_t after = (uint16_t)(pc + 3U);

                if (jump_count >=
                    (uint16_t)(sizeof(jump_target) / sizeof(jump_target[0])))
                    return false;

                jump_target[jump_count++] =
                    (int32_t)after + (int32_t)rel;

                pc = after;
                break;
            }

            case OP_JMP: {
                if ((uint16_t)(pc + 2U) > len)
                    return false;

                int16_t rel = read_i16_le(&code[pc]);
                uint16_t after = (uint16_t)(pc + 2U);

                if (jump_count >=
                    (uint16_t)(sizeof(jump_target) / sizeof(jump_target[0])))
                    return false;

                jump_target[jump_count++] =
                    (int32_t)after + (int32_t)rel;

                pc = after;
                break;
            }

            case OP_WAIT_S:
                if ((uint16_t)(pc + 4U) > len)
                    return false;
                pc += 4U;
                break;

            case OP_WAIT_EVENT:
                if ((uint16_t)(pc + 3U) > len)
                    return false;
                if (code[pc+1U] >= VM_REG_COUNT || code[pc+2U] >= VM_REG_COUNT)
                    return false;
                pc += 3U;
                break;

            case OP_WAIT_HALL:
            case OP_DEBUG_I32:
            case OP_BAT_READ:
            case OP_DEBUG_BAT:
            case OP_LORA_SEND_BAT:
                if (pc >= len || code[pc] >= VM_REG_COUNT)
                    return false;
                pc++;
                break;

            case OP_NOTIFY_U32:
                if ((uint16_t)(pc + 2U) > len || code[pc + 1U] >= VM_REG_COUNT)
                    return false;
                pc += 2U;
                break;

            case OP_NOTIFY_STR: {
                if ((uint16_t)(pc + 2U) > len)
                    return false;
                uint8_t slen = code[pc + 1U];
                if (slen == 0U || slen > NRFCLAW_VM_MAX_NOTIFY_STRING)
                    return false;
                pc += 2U;
                if ((uint16_t)(pc + slen) > len)
                    return false;
                pc = (uint16_t)(pc + slen);
                break;
            }

            case OP_STATE_SET:
            case OP_STATE_GET:
            case OP_PERSIST_SAVE:
            case OP_PERSIST_LOAD:
                if ((uint16_t)(pc + 2U) > len || code[pc] >= NRFCLAW_STATE_USER_COUNT || code[pc+1U] >= VM_REG_COUNT)
                    return false;
                pc += 2U;
                break;

            case OP_PERSIST_LOAD_DEFAULT:
                if ((uint16_t)(pc + 6U) > len || code[pc] >= NRFCLAW_STATE_USER_COUNT || code[pc+1U] >= VM_REG_COUNT)
                    return false;
                pc = (uint16_t)(pc + 6U);
                break;

            case OP_GPIO_WRITE_IMM:
                if ((uint16_t)(pc + 2U) > len) return false;
                pc += 2U;
                break;

            case OP_GPIO_WRITE_REG:
            case OP_GPIO_READ:
                if ((uint16_t)(pc + 2U) > len || code[pc+1U] >= VM_REG_COUNT)
                    return false;
                pc += 2U;
                break;

            case OP_ACCEL_READ:
                if ((uint16_t)(pc + 3U) > len ||
                    code[pc] >= VM_REG_COUNT || code[pc+1U] >= VM_REG_COUNT || code[pc+2U] >= VM_REG_COUNT)
                    return false;
                pc += 3U;
                break;

            case OP_ACCEL_MOTION:
                if ((uint16_t)(pc + 3U) > len) return false;
                pc += 3U;
                break;

            case OP_ACCEL_CONFIG:
                if ((uint16_t)(pc + 9U) > len) return false;
                pc += 9U;
                break;

            case OP_DS18_READ:
                if (pc >= len || code[pc] >= VM_REG_COUNT) return false;
                pc++;
                break;

            case OP_VIB_MODEL_SET:
                if ((uint16_t)(pc + 16U) > len) return false;
                pc += 16U;
                break;

            case OP_VIB_SCORE:
                if (pc >= len || code[pc] >= VM_REG_COUNT) return false;
                pc++;
                break;

            case OP_CAP_AVAILABLE:
            case OP_APP_GET_U32:
                if ((uint16_t)(pc + 2U) > len || code[pc+1U] >= VM_REG_COUNT)
                    return false;
                pc += 2U;
                break;

            case OP_APP_SEND_U32:
                if ((uint16_t)(pc + 5U) > len || code[pc+3U] >= VM_REG_COUNT || code[pc+4U] >= VM_REG_COUNT)
                    return false;
                pc += 5U;
                break;

            case OP_SERIAL_CONFIG:
            case OP_SERIAL_CONFIG_ECO:
                /*
                 * tx_pin:u8, rx_pin:u8, baud:u32-le
                 */
                if ((uint16_t)(pc + 6U) > len)
                    return false;
                pc += 6U;
                break;

            case OP_SERIAL_DISABLE:
                break;

            case OP_SERIAL_WRITE: {
                if (pc >= len)
                    return false;

                uint8_t slen = code[pc++];

                if (slen == 0U ||
                    slen > 64U ||
                    (uint16_t)(pc + slen) > len)
                    return false;

                pc = (uint16_t)(pc + slen);
                break;
            }

            case OP_SERIAL_READ:
                if (pc >= len || code[pc] >= VM_REG_COUNT)
                    return false;
                pc++;
                break;

            case OP_TRACKING_SET_KEY:
                if ((uint16_t)(pc + NRFCLAW_TRACKING_KEY_SIZE) > len)
                    return false;
                pc = (uint16_t)(pc + NRFCLAW_TRACKING_KEY_SIZE);
                break;

            case OP_TRACKING_CONFIG:
                /* interval_ms:u16 + tx_power_dbm:i8 */
                if ((uint16_t)(pc + 3U) > len)
                    return false;
                pc += 3U;
                break;

            case OP_TRACKING_START:
            case OP_TRACKING_STOP:
                break;

            case OP_TRACKING_STATUS:
                if (pc >= len || code[pc] >= VM_REG_COUNT)
                    return false;
                pc++;
                break;

            case OP_TRACKING_ROTATION_SET:
                if ((uint16_t)(pc + 4U) > len)
                    return false;
                pc += 4U;
                break;

            case OP_LORA_SEND_BYTES: {
                if (pc >= len) return false;
                uint8_t plen = code[pc++];
                if (plen == 0U || plen > 32U || (uint16_t)(pc + plen) > len)
                    return false;
                pc = (uint16_t)(pc + plen);
                break;
            }

            case OP_LORA_CONFIG:
                if ((uint16_t)(pc + 9U) > len) return false;
                pc += 9U;
                break;

            case OP_FORMAT_REG: {
                if (pc >= len) return false;
                uint8_t plen = code[pc++];
                if (plen > 48U || (uint16_t)(pc + plen + 2U) > len) return false;
                pc = (uint16_t)(pc + plen);
                if (code[pc] >= VM_REG_COUNT || code[pc + 1U] > 4U) return false;
                pc += 2U;
                break;
            }

            case OP_FORMAT_2REG: {
                if (pc >= len) return false;
                uint8_t p1 = code[pc++];
                if (p1 > 32U || (uint16_t)(pc + p1 + 3U) > len) return false;
                pc = (uint16_t)(pc + p1);
                if (code[pc] >= VM_REG_COUNT || code[pc + 1U] > 4U) return false;
                pc += 2U;
                uint8_t p2 = code[pc++];
                if (p2 > 32U || (uint16_t)(pc + p2 + 2U) > len) return false;
                pc = (uint16_t)(pc + p2);
                if (code[pc] >= VM_REG_COUNT || code[pc + 1U] > 4U) return false;
                pc += 2U;
                break;
            }

            case OP_LORA_LAST_RSSI:
            case OP_PARSE_BUF_I32:
                if (pc >= len || code[pc] >= VM_REG_COUNT) return false;
                pc += 1U;
                break;

            case OP_PARSE_BUF_FIXED:
                if ((uint16_t)(pc + 2U) > len || code[pc] >= VM_REG_COUNT || code[pc+1U] > 6U) return false;
                pc += 2U;
                break;

            case OP_FORMAT_REG_FIXED: {
                if (pc >= len) return false;
                uint8_t plen=code[pc++];
                if (plen>48U || (uint16_t)(pc+plen+3U)>len) return false;
                pc=(uint16_t)(pc+plen);
                if (code[pc] >= VM_REG_COUNT || code[pc+1U] > 6U) return false;
                pc += 2U;
                uint8_t slen=code[pc++];
                if (slen>16U || (uint16_t)(pc+slen)>len) return false;
                pc=(uint16_t)(pc+slen);
                break;
            }

            case OP_BUFFER_PREPEND: {
                if (pc >= len) return false;
                uint8_t plen = code[pc++];
                if (plen == 0U || plen > 48U || (uint16_t)(pc + plen) > len) return false;
                pc = (uint16_t)(pc + plen);
                break;
            }

            /* B4.12 fixed-phase NinaLink telemetry config. */
            case OP_NINALINK_TELEMETRY_SYNTH: {
                uint16_t period_s, window_ms, backoff_ms;
                uint8_t attempts;
                int32_t temperature_mC;
                if ((uint16_t)(pc + 11U) > len) return false;
                period_s=(uint16_t)code[pc]|((uint16_t)code[pc+1U]<<8);
                window_ms=(uint16_t)code[pc+2U]|((uint16_t)code[pc+3U]<<8);
                attempts=code[pc+4U];
                backoff_ms=(uint16_t)code[pc+5U]|((uint16_t)code[pc+6U]<<8);
                temperature_mC=(int32_t)((uint32_t)code[pc+7U]|((uint32_t)code[pc+8U]<<8)|((uint32_t)code[pc+9U]<<16)|((uint32_t)code[pc+10U]<<24));
                if (period_s<5U || period_s>300U || window_ms<100U || window_ms>4000U || attempts==0U || attempts>5U || (attempts>1U && (backoff_ms==0U || backoff_ms>4000U)) || temperature_mC<-55000L || temperature_mC>125000L) return false;
                pc=(uint16_t)(pc+11U); break;
            }

            case OP_NINALINK_TELEMETRY: {
                uint16_t period_s;
                uint16_t window_ms;
                uint8_t attempts;
                uint16_t backoff_ms;

                if ((uint16_t)(pc + 7U) > len)
                    return false;

                period_s = (uint16_t)code[pc] |
                    ((uint16_t)code[pc + 1U] << 8);
                window_ms = (uint16_t)code[pc + 2U] |
                    ((uint16_t)code[pc + 3U] << 8);
                attempts = code[pc + 4U];
                backoff_ms = (uint16_t)code[pc + 5U] |
                    ((uint16_t)code[pc + 6U] << 8);

                if (period_s < 5U || period_s > 300U ||
                    window_ms < 100U || window_ms > 4000U ||
                    attempts == 0U || attempts > 5U ||
                    (attempts > 1U &&
                     (backoff_ms == 0U || backoff_ms > 4000U)))
                    return false;

                pc = (uint16_t)(pc + 7U);
                break;
            }

            case OP_HA_ROLE_CONFIG:
                if (pc >= len || code[pc] > NRFCLAW_HA_ROLE_BRIDGE)
                    return false;
                pc += 1U;
                break;

            case OP_HA_NINALINK_NODE_CONFIG: {
                uint16_t period_s;
                if ((uint16_t)(pc + 2U) > len)
                    return false;
                period_s = (uint16_t)code[pc] |
                    ((uint16_t)code[pc + 1U] << 8);
                if (period_s < NRFCLAW_HA_NODE_MIN_PERIOD_S ||
                    period_s > NRFCLAW_HA_NODE_MAX_PERIOD_S)
                    return false;
                pc = (uint16_t)(pc + 2U);
                break;
            }

            case OP_LORA_PROFILE_PERSIST: {
                uint8_t mask;
                if ((uint16_t)(pc + 10U) > len)
                    return false;
                mask = code[pc];
                if (mask == 0U || (mask & 0xE0U) != 0U)
                    return false;
                pc = (uint16_t)(pc + 10U);
                break;
            }

            case OP_SEMANTIC_PUBLISH: {
                nrfclaw_capability_desc_t d;
                uint16_t cap;
                if ((uint16_t)(pc + 4U) > len || code[pc + 3U] >= VM_REG_COUNT)
                    return false;
                cap = (uint16_t)code[pc] | ((uint16_t)code[pc + 1U] << 8);
                if (!nrfclaw_capability_descriptor(cap, code[pc + 2U], &d))
                    return false;
                if ((d.behavior_flags & (NRFCLAW_CAP_BEHAVIOR_REPORTABLE | NRFCLAW_CAP_BEHAVIOR_RETAINED)) !=
                    (NRFCLAW_CAP_BEHAVIOR_REPORTABLE | NRFCLAW_CAP_BEHAVIOR_RETAINED))
                    return false;
                pc = (uint16_t)(pc + 4U);
                break;
            }

            case OP_BUFFER_SET: {
                if (pc >= len) return false;
                uint8_t blen = code[pc++];
                if (blen == 0U || blen > NRFCLAW_VM_BUFFER_SIZE || (uint16_t)(pc + blen) > len) return false;
                pc = (uint16_t)(pc + blen);
                break;
            }

            case OP_LORA_SEND_BUF:
            case OP_SERIAL_WRITE_BUF:
            case OP_BLE_ADV_BUF:
            case OP_DEBUG_BUFFER:
            case OP_APP_SEND_BUF:
                break;

            case OP_SERIAL_RX_BUF:
                if ((uint16_t)(pc + 3U) > len) return false;
                if (code[pc] > 2U) return false;
                if (code[pc] == 2U) {
                    uint16_t n = (uint16_t)code[pc + 1U] | ((uint16_t)code[pc + 2U] << 8);
                    if (n == 0U || n > NRFCLAW_VM_BUFFER_SIZE) return false;
                }
                pc += 3U;
                break;

            case OP_LORA_RX_START:
                if (pc >= len) return false;
                pc++;
                break;

            case OP_APP_EVENT_SEND:
                if ((uint16_t)(pc + 3U) > len || code[pc + 2U] >= VM_REG_COUNT) return false;
                pc += 3U;
                break;

            case OP_BLE_APP_ROLE:
                if (pc >= len) return false;
                pc++;
                break;

            case OP_BLE_ADV_CONFIG: {
                if ((uint16_t)(pc + 4U) > len) return false;
                uint8_t plen = code[pc + 3U];
                pc += 4U;
                if (plen > 31U || (uint16_t)(pc + plen) > len) return false;
                pc = (uint16_t)(pc + plen);
                break;
            }

            case OP_BLE_ADV_NAME: {
                if (pc >= len) return false;
                uint8_t nlen = code[pc++];
                if (nlen == 0U || nlen > 24U || (uint16_t)(pc + nlen) > len) return false;
                pc = (uint16_t)(pc + nlen);
                break;
            }

            case OP_BLE_ADV_START:
            case OP_BLE_ADV_STOP:
            case OP_BLE_DISCONNECT:
                break;

            case OP_BLE_CONNECT:
                if ((uint16_t)(pc + 6U) > len) return false;
                pc += 6U;
                break;

            case OP_VIB_AUTO_START:
                if (pc >= len) return false;
                pc++;
                break;

            case OP_VIB_AUTO_STOP:
                break;

            case OP_VIB_AUTO_CONFIG_TIME:
                if ((uint16_t)(pc + 8U) > len) return false;
                pc += 8U;
                break;

            case OP_HALL_CONFIG:
                if ((uint16_t)(pc + 5U) > len) return false;
                pc += 5U;
                break;

            case OP_SYSTEM_MIN_POWER:
                break;

            default:
                return false;
        }
    }

    return false;
}

static void runtime_reset(void)
{
    m_pc = 0;
    memset(m_reg, 0, sizeof(m_reg));
    memset(m_reg_state_key_valid, 0, sizeof(m_reg_state_key_valid));
    memset(m_reg_state_key, 0, sizeof(m_reg_state_key));
    m_semantic_accumulator_bound = false;
    m_semantic_accumulator_reg = 0U;
    m_semantic_accumulator_key = 0U;
    m_semantic_accumulator_capability = 0U;
    m_semantic_accumulator_channel = 0U;
    m_semantic_reset_restart_pending = false;
    memset(m_buffer, 0, sizeof(m_buffer));
    m_buffer_len = 0U;
    m_app_event_pending = false;
    nrfclaw_vm_semantic_state_clear();
    m_pending_battery_reg = 0;
    m_pending_hall_reg = 0;
    m_pending_event_type = 0;
    m_pending_event_reg0 = 0;
    m_pending_event_reg1 = 0;
}

/* -------------------------------------------------------------------------
 * Public runtime API
 * ------------------------------------------------------------------------- */
void nrfclaw_vm_set_notify_handler(nrfclaw_vm_notify_handler_t handler)
{
    m_notify_handler = handler;
}

void nrfclaw_vm_init(void)
{
    m_state = NRFCLAW_VM_STOPPED;
    m_code = NULL;
    m_code_len = 0;
    runtime_reset();

    m_program_len = 0;
    m_program_loaded = false;

    m_upload_active = false;
    m_upload_expected_len = 0;
    m_upload_expected_crc = 0;
    m_upload_written = 0;
    memset(m_upload_auth_tag, 0, sizeof(m_upload_auth_tag));
    m_upload_auth_written = 0;
    memset(&m_upload_schedule, 0, sizeof(m_upload_schedule));
    m_upload_schedule.mode = NRFCLAW_SCHEDULE_MANUAL;
}

nrfclaw_vm_state_t nrfclaw_vm_state(void)
{
    return m_state;
}

uint16_t nrfclaw_vm_pc(void)
{
    return m_pc;
}

uint16_t nrfclaw_vm_loaded_program_len(void)
{
    return m_program_loaded ? m_program_len : 0U;
}

bool nrfclaw_vm_stop(void)
{
    /* Stage 3 stops VM execution. Native drivers already in flight are
     * allowed to finish/cleanup through the event loop. */
    m_state = NRFCLAW_VM_STOPPED;
    return true;
}

bool nrfclaw_vm_run_loaded(void)
{
    if (!m_program_loaded)
        return false;

    /*
     * Do not replace a currently executing/suspended program.
     * RUN is accepted only from a quiescent VM state.
     */
    if (m_state != NRFCLAW_VM_STOPPED &&
        m_state != NRFCLAW_VM_DONE &&
        m_state != NRFCLAW_VM_ERROR)
        return false;

    m_code = m_program;
    m_code_len = m_program_len;
    runtime_reset();
    m_state = NRFCLAW_VM_READY;

    ((void)0);
    return true;
}


/* B7.6f2l3: reset the currently published retained accumulator. The VM is
 * paused while the state journal commits zero, then the loaded program is
 * restarted so it reloads zero and cannot resurrect the pre-reset RAM value
 * on the next physical event. */
bool nrfclaw_vm_semantic_accumulator_resettable(void)
{
    nrfclaw_vm_semantic_state_t semantic;
    if (!m_semantic_accumulator_bound ||
        m_semantic_accumulator_reg >= VM_REG_COUNT ||
        m_semantic_accumulator_key >= NRFCLAW_STATE_USER_COUNT ||
        !nrfclaw_vm_semantic_state_snapshot(&semantic))
        return false;
    return semantic.capability_id == m_semantic_accumulator_capability &&
           semantic.channel == m_semantic_accumulator_channel;
}

bool nrfclaw_vm_semantic_accumulator_reset(void)
{
    if (!nrfclaw_vm_semantic_accumulator_resettable() ||
        nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;
    /* nrfclaw_state_persist() returns false when it has to start page
     * compaction, even though the new RAM value is already installed and the
     * compaction will persist it.  From an IDLE precondition, SAVING therefore
     * also means the reset was accepted. */
    if (!nrfclaw_state_persist(m_semantic_accumulator_key, 0U) &&
        nrfclaw_state_status() != NRFCLAW_STATE_SAVING)
        return false;

    /* Stop before another physical event can consume the old accumulator.
     * The retained semantic value is intentionally republished by the BOOT
     * program only after the Flash transaction commits, making the observable
     * zero and the persisted zero one atomic behavioral transition. */
    m_reg[m_semantic_accumulator_reg] = 0U;
    m_semantic_reset_restart_pending = true;
    m_state = NRFCLAW_VM_STOPPED;
    return true;
}


bool nrfclaw_vm_install_program(uint8_t const *program,
                                uint16_t len)
{
    if (!program ||
        len == 0U ||
        len > NRFCLAW_VM_MAX_PROGRAM_SIZE)
        return false;

    if (!validate_program(program, len))
        return false;

    memcpy(m_program, program, len);
    m_program_len = len;
    m_program_loaded = true;

    m_code = NULL;
    m_code_len = 0;
    runtime_reset();
    m_state = NRFCLAW_VM_STOPPED;

    ((void)0);

    return true;
}

/* -------------------------------------------------------------------------
 * Stage-3 program loader
 * ------------------------------------------------------------------------- */
nrfclaw_upload_result_t nrfclaw_vm_upload_begin(uint16_t total_len,
                                                 uint16_t expected_crc)
{
    if (m_state == NRFCLAW_VM_READY ||
        m_state == NRFCLAW_VM_WAIT_RTC ||
        m_state == NRFCLAW_VM_WAIT_HALL ||
        m_state == NRFCLAW_VM_WAIT_BATTERY ||
        m_state == NRFCLAW_VM_WAIT_LORA ||
        m_state == NRFCLAW_VM_WAIT_EVENT ||
        m_state == NRFCLAW_VM_WAIT_SERIAL_TX ||
        m_state == NRFCLAW_VM_WAIT_SERIAL_RX)
        return NRFCLAW_UPLOAD_BUSY;

    if (total_len == 0U || total_len > NRFCLAW_VM_MAX_PROGRAM_SIZE)
        return NRFCLAW_UPLOAD_SIZE;

    /*
     * Stage 3 has a single RAM slot. Starting a new upload invalidates
     * the previous loaded image immediately, so a failed/partial upload can
     * never leave a corrupted old program marked as executable.
     */
    m_program_loaded = false;
    m_program_len = 0;

    m_upload_active = true;
    m_upload_expected_len = total_len;
    m_upload_expected_crc = expected_crc;
    m_upload_written = 0;
    memset(m_upload_auth_tag, 0, sizeof(m_upload_auth_tag));
    m_upload_auth_written = 0;
    memset(&m_upload_schedule, 0, sizeof(m_upload_schedule));
    m_upload_schedule.mode = NRFCLAW_SCHEDULE_MANUAL;

    return NRFCLAW_UPLOAD_OK;
}

nrfclaw_upload_result_t nrfclaw_vm_upload_write(uint16_t offset,
                                                 uint8_t const *data,
                                                 uint16_t len)
{
    if (!m_upload_active)
        return NRFCLAW_UPLOAD_BUSY;

    if (!data || len == 0U)
        return NRFCLAW_UPLOAD_SIZE;

    /* Sequential-only Stage-3 transport: simpler, smaller and prevents
     * ambiguous overlapping fragments. */
    if (offset != m_upload_written)
        return NRFCLAW_UPLOAD_OFFSET;

    if ((uint32_t)offset + len > m_upload_expected_len)
        return NRFCLAW_UPLOAD_SIZE;

    memcpy(&m_program[offset], data, len);
    m_upload_written = (uint16_t)(m_upload_written + len);

    return NRFCLAW_UPLOAD_OK;
}

nrfclaw_upload_result_t nrfclaw_vm_upload_schedule(
    nrfclaw_schedule_t const *schedule)
{
    if (!m_upload_active)
        return NRFCLAW_UPLOAD_BUSY;

    if (!schedule)
        return NRFCLAW_UPLOAD_SIZE;

    switch (schedule->mode)
    {
        case NRFCLAW_SCHEDULE_MANUAL:
        case NRFCLAW_SCHEDULE_BOOT:
            break;

        case NRFCLAW_SCHEDULE_AT:
            if (schedule->arg0 == 0U)
                return NRFCLAW_UPLOAD_SIZE;
            break;

        case NRFCLAW_SCHEDULE_EVERY:
            if (schedule->arg0 == 0U ||
                schedule->arg0 >= 0x00FFFF00UL)
                return NRFCLAW_UPLOAD_SIZE;
            break;

        case NRFCLAW_SCHEDULE_WEEKLY:
            if ((schedule->dow_mask & 0x7FU) == 0U ||
                schedule->arg0 >= 86400UL)
                return NRFCLAW_UPLOAD_SIZE;
            break;

        default:
            return NRFCLAW_UPLOAD_SIZE;
    }

    m_upload_schedule = *schedule;
    m_upload_schedule.dow_mask &= 0x7FU;
    m_upload_schedule.reserved = 0U;

    return NRFCLAW_UPLOAD_OK;
}

nrfclaw_upload_result_t nrfclaw_vm_upload_auth_write(
    uint16_t offset,
    uint8_t const *data,
    uint16_t len)
{
    if (!m_upload_active)
        return NRFCLAW_UPLOAD_BUSY;

    if (!data || len == 0U)
        return NRFCLAW_UPLOAD_SIZE;

    if (offset != m_upload_auth_written)
        return NRFCLAW_UPLOAD_OFFSET;

    if ((uint32_t)offset + len > NRFCLAW_AUTH_TAG_SIZE)
        return NRFCLAW_UPLOAD_SIZE;

    memcpy(&m_upload_auth_tag[offset], data, len);

    m_upload_auth_written =
        (uint8_t)(m_upload_auth_written + len);

    return NRFCLAW_UPLOAD_OK;
}

nrfclaw_upload_result_t nrfclaw_vm_upload_finish(void)
{
    if (!m_upload_active)
        return NRFCLAW_UPLOAD_BUSY;

    if (m_upload_written != m_upload_expected_len)
        return NRFCLAW_UPLOAD_INCOMPLETE;

    if (crc16_ccitt(m_program, m_upload_expected_len) !=
        m_upload_expected_crc) {
        m_upload_active = false;
        return NRFCLAW_UPLOAD_CRC;
    }

    if (m_upload_auth_written != NRFCLAW_AUTH_TAG_SIZE)
    {
        return NRFCLAW_UPLOAD_AUTH_REQUIRED;
    }

    if (!nrfclaw_auth_verify(
            m_program,
            m_upload_expected_len,
            &m_upload_schedule,
            m_upload_auth_tag))
    {
        m_upload_active = false;
        return NRFCLAW_UPLOAD_AUTH_FAILED;
    }

    if (!validate_program(m_program, m_upload_expected_len)) {
        m_upload_active = false;
        return NRFCLAW_UPLOAD_INVALID_PROGRAM;
    }

    /*
     * Stage 4: request atomic persistence before finalizing PROGRAM_END.
     * nrfclaw_flash_request_save() copies the current RAM program into its
     * own stable staging buffer, so subsequent VM activity cannot corrupt
     * an in-flight SoftDevice flash write.
     */
    if (!nrfclaw_flash_request_save(
            m_program,
            m_upload_expected_len,
            m_upload_expected_crc,
            &m_upload_schedule,
            m_upload_auth_tag))
    {
        /*
         * Keep upload active so PROGRAM_END can be retried.
         */
        return NRFCLAW_UPLOAD_BUSY;
    }

    m_program_len = m_upload_expected_len;
    m_program_loaded = true;
    m_upload_active = false;

    /* BOOT programs must only start after reboot, never inside the NUS
     * programming session that just uploaded them. */
    (void)nrfclaw_scheduler_set_rule_deferred(
        &m_upload_schedule
    );

    ((void)0);

    return NRFCLAW_UPLOAD_OK;
}

/* -------------------------------------------------------------------------
 * Stage-2 regression helper
 * ------------------------------------------------------------------------- */
bool nrfclaw_vm_start_validation_test(uint8_t test_id)
{
    switch (test_id) {
        case 2:
            m_code = m_test2_program;
            m_code_len = sizeof(m_test2_program);
            break;
        case 3:
            m_code = m_test3_program;
            m_code_len = sizeof(m_test3_program);
            break;
        case 4:
            m_code = m_test4_program;
            m_code_len = sizeof(m_test4_program);
            break;
        case 5:
            m_code = m_test5_program;
            m_code_len = sizeof(m_test5_program);
            break;
        default:
            return false;
    }

    runtime_reset();
    m_state = NRFCLAW_VM_READY;
    return true;
}

/* -------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------- */
void nrfclaw_vm_on_event(nrfclaw_event_t const *e)
{
    if (!e)
        return;

    if (m_state == NRFCLAW_VM_WAIT_EVENT &&
        (m_pending_event_type == 0U ||
         m_pending_event_type == (uint8_t)e->type)) {
        if (m_pending_event_reg0 >= VM_REG_COUNT ||
            m_pending_event_reg1 >= VM_REG_COUNT) {
            vm_error("invalid WAIT_EVENT destination register");
            return;
        }
        m_reg[m_pending_event_reg0] = e->arg0;
        m_reg[m_pending_event_reg1] = e->arg1;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_RTC &&
        m_state == NRFCLAW_VM_WAIT_RTC) {
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_HALL &&
        m_state == NRFCLAW_VM_WAIT_HALL) {
        if (m_pending_hall_reg >= VM_REG_COUNT) {
            vm_error("invalid HALL destination register");
            return;
        }

        m_reg[m_pending_hall_reg] = e->arg0;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_BATTERY_DONE &&
        m_state == NRFCLAW_VM_WAIT_BATTERY) {
        if (m_pending_battery_reg >= VM_REG_COUNT) {
            vm_error("invalid battery destination register");
            return;
        }

        m_reg[m_pending_battery_reg] = e->arg0;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_TEMPERATURE_DONE &&
        m_state == NRFCLAW_VM_WAIT_DS18B20) {
        if (m_pending_ds18_reg >= VM_REG_COUNT) { vm_error("invalid DS18 destination register"); return; }
        m_reg[m_pending_ds18_reg] = e->arg0;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_SERIAL_TX_DONE &&
        m_state == NRFCLAW_VM_WAIT_SERIAL_TX)
    {
        /*
         * SERIAL_WRITE already advanced PC past its payload before starting
         * TX. Resume at the next opcode only after the UART driver confirms
         * every byte has left the peripheral.
         */
        m_state =
            NRFCLAW_VM_READY;

        return;
    }


    if (e->type == NRFCLAW_EVT_SERIAL_FRAME &&
        m_state == NRFCLAW_VM_WAIT_SERIAL_RX)
    {
        uint8_t len = 0U;
        if (!nrfclaw_serial_frame_take(m_buffer, sizeof(m_buffer), &len) || len == 0U) {
            vm_error("SERIAL frame unavailable");
            return;
        }
        m_buffer_len = len;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_APP_CONNECTED) {
        app_event_try_send();
    }

    if (e->type == NRFCLAW_EVT_LORA_RX_DONE &&
        m_state == NRFCLAW_VM_WAIT_LORA_RX) {
        uint8_t len = 0U;
        if (!nrfclaw_lora_take_rx(m_buffer, &len, sizeof(m_buffer))) {
            vm_error("LoRa RX buffer unavailable");
            return;
        }
        m_buffer_len = len;
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_LORA_TX_DONE &&
        m_state == NRFCLAW_VM_WAIT_LORA) {
        m_state = NRFCLAW_VM_READY;
        return;
    }

    if (e->type == NRFCLAW_EVT_LORA_TIMEOUT &&
        m_state == NRFCLAW_VM_WAIT_LORA) {
        vm_error("LoRa TX timeout");
        return;
    }
}

/* -------------------------------------------------------------------------
 * Executor
 * ------------------------------------------------------------------------- */
void nrfclaw_vm_tick(void)
{
    if (m_semantic_reset_restart_pending) {
        if (nrfclaw_state_status() == NRFCLAW_STATE_ERROR) {
            m_semantic_reset_restart_pending = false;
            m_state = NRFCLAW_VM_ERROR;
            return;
        }
        if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
            return;
        m_semantic_reset_restart_pending = false;
        if (!nrfclaw_vm_run_loaded()) {
            m_state = NRFCLAW_VM_ERROR;
            return;
        }
    }

    app_event_try_send();
    uint8_t steps = 0;

    while (m_state == NRFCLAW_VM_READY &&
           steps++ < VM_MAX_STEPS_PER_TICK) {
        if (!m_code || m_pc >= m_code_len) {
            vm_error("PC outside program");
            return;
        }

        uint8_t op = m_code[m_pc++];

        switch (op) {
            case OP_END:
                m_state = NRFCLAW_VM_DONE;
                ((void)0);
                return;

            case OP_MOV: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated MOV"); return;
                }
                uint8_t dst = m_code[m_pc++];
                uint8_t src = m_code[m_pc++];
                if (dst >= VM_REG_COUNT || src >= VM_REG_COUNT) {
                    vm_error("invalid MOV register"); return;
                }
                m_reg[dst] = m_reg[src];
                break;
            }

            case OP_NEG: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated NEG"); return;
                }
                uint8_t dst = m_code[m_pc++];
                uint8_t src = m_code[m_pc++];
                if (dst >= VM_REG_COUNT || src >= VM_REG_COUNT) {
                    vm_error("invalid NEG register"); return;
                }
                /* Defined two's-complement wrap, including INT32_MIN. */
                m_reg[dst] = 0U - m_reg[src];
                break;
            }

            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD:
            case OP_CMP_EQ:
            case OP_CMP_NE:
            case OP_CMP_LT:
            case OP_CMP_LE:
            case OP_CMP_GT: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) {
                    vm_error("truncated expression opcode"); return;
                }

                uint8_t dst = m_code[m_pc++];
                uint8_t a   = m_code[m_pc++];
                uint8_t b   = m_code[m_pc++];

                if (dst >= VM_REG_COUNT ||
                    a >= VM_REG_COUNT ||
                    b >= VM_REG_COUNT) {
                    vm_error("invalid expression register"); return;
                }

                uint32_t ua = m_reg[a];
                uint32_t ub = m_reg[b];
                int32_t sa = (int32_t)ua;
                int32_t sb = (int32_t)ub;

                switch (op) {
                    case OP_ADD:
                        m_reg[dst] = ua + ub;
                        break;

                    case OP_SUB:
                        m_reg[dst] = ua - ub;
                        break;

                    case OP_MUL:
                        m_reg[dst] = ua * ub;
                        break;

                    case OP_DIV:
                        if (sb == 0) {
                            vm_error("division by zero"); return;
                        }
                        if (sa == INT32_MIN && sb == -1)
                            m_reg[dst] = 0x80000000UL;
                        else
                            m_reg[dst] = (uint32_t)(sa / sb);
                        break;

                    case OP_MOD:
                        if (sb == 0) {
                            vm_error("modulo by zero"); return;
                        }
                        if (sa == INT32_MIN && sb == -1)
                            m_reg[dst] = 0U;
                        else
                            m_reg[dst] = (uint32_t)(sa % sb);
                        break;

                    case OP_CMP_EQ:
                        m_reg[dst] = (ua == ub) ? 1U : 0U;
                        break;

                    case OP_CMP_NE:
                        m_reg[dst] = (ua != ub) ? 1U : 0U;
                        break;

                    case OP_CMP_LT:
                        m_reg[dst] = (sa < sb) ? 1U : 0U;
                        break;

                    case OP_CMP_LE:
                        m_reg[dst] = (sa <= sb) ? 1U : 0U;
                        break;

                    case OP_CMP_GT:
                        m_reg[dst] = (sa > sb) ? 1U : 0U;
                        break;

                    default:
                        vm_error("internal expression dispatch"); return;
                }
                break;
            }

            case OP_MOVI: {
                if ((uint16_t)(m_pc + 5U) > m_code_len) {
                    vm_error("truncated MOVI"); return;
                }
                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) { vm_error("invalid MOVI register"); return; }
                m_reg[reg] = read_u32_le(&m_code[m_pc]);
                m_pc += 4U;
                break;
            }

            case OP_CMP_GE: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) {
                    vm_error("truncated CMP_GE"); return;
                }
                uint8_t dst=m_code[m_pc++], a=m_code[m_pc++], b=m_code[m_pc++];
                if (dst>=VM_REG_COUNT || a>=VM_REG_COUNT || b>=VM_REG_COUNT) {
                    vm_error("invalid CMP_GE register"); return;
                }
                m_reg[dst] = ((int32_t)m_reg[a] >= (int32_t)m_reg[b]) ? 1U : 0U;
                break;
            }

            case OP_JNZ: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) {
                    vm_error("truncated JNZ"); return;
                }
                uint8_t reg=m_code[m_pc++];
                int16_t rel=read_i16_le(&m_code[m_pc]); m_pc+=2U;
                if (reg>=VM_REG_COUNT) { vm_error("invalid JNZ register"); return; }
                if (m_reg[reg] != 0U && !jump_relative(rel)) {
                    vm_error("JNZ target outside program"); return;
                }
                break;
            }

            case OP_JZ: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) {
                    vm_error("truncated JZ"); return;
                }
                uint8_t reg=m_code[m_pc++];
                int16_t rel=read_i16_le(&m_code[m_pc]); m_pc+=2U;
                if (reg>=VM_REG_COUNT) { vm_error("invalid JZ register"); return; }
                if (m_reg[reg] == 0U && !jump_relative(rel)) {
                    vm_error("JZ target outside program"); return;
                }
                break;
            }

            case OP_JMP: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated JMP"); return;
                }
                int16_t rel=read_i16_le(&m_code[m_pc]); m_pc+=2U;
                if (!jump_relative(rel)) { vm_error("JMP target outside program"); return; }
                break;
            }

            case OP_WAIT_S: {
                if ((uint16_t)(m_pc + 4U) > m_code_len) {
                    vm_error("truncated WAIT_S");
                    return;
                }

                uint32_t seconds = read_u32_le(&m_code[m_pc]);
                m_pc += 4U;

                if (seconds == 0U)
                    break;

                if (!nrfclaw_rtc_set_alarm_epoch(
                        nrfclaw_rtc_now() + seconds)) {
                    vm_error("RTC alarm rejected");
                    return;
                }

                m_state = NRFCLAW_VM_WAIT_RTC;
                return;
            }

            case OP_WAIT_EVENT: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) {
                    vm_error("truncated WAIT_EVENT"); return;
                }
                uint8_t event_type=m_code[m_pc++];
                uint8_t r0=m_code[m_pc++];
                uint8_t r1=m_code[m_pc++];
                if (r0>=VM_REG_COUNT || r1>=VM_REG_COUNT) {
                    vm_error("invalid WAIT_EVENT register"); return;
                }
                m_pending_event_type=event_type;
                m_pending_event_reg0=r0;
                m_pending_event_reg1=r1;
                m_state=NRFCLAW_VM_WAIT_EVENT;
                return;
            }

            case OP_WAIT_HALL: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated WAIT_HALL");
                    return;
                }

                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid WAIT_HALL register");
                    return;
                }

                m_pending_hall_reg = reg;
                m_state = NRFCLAW_VM_WAIT_HALL;
                return;
            }

            case OP_DEBUG_I32: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated DEBUG_I32");
                    return;
                }

                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid DEBUG_I32 register");
                    return;
                }
                break;
            }

            case OP_BAT_READ: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated BAT_READ");
                    return;
                }

                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid BAT_READ register");
                    return;
                }

                if (!nrfclaw_battery_start()) {
                    vm_error("battery driver busy");
                    return;
                }

                m_pending_battery_reg = reg;
                m_state = NRFCLAW_VM_WAIT_BATTERY;
                return;
            }

            case OP_DEBUG_BAT: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated DEBUG_BAT");
                    return;
                }

                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid DEBUG_BAT register");
                    return;
                }

                uint32_t cv = m_reg[reg];
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "VM: BAT R%u=%lu.%02luV\r\n",
                         reg,
                         (unsigned long)(cv / 100U),
                         (unsigned long)(cv % 100U));
                ((void)0);
                break;
            }

            case OP_NOTIFY_U32: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated NOTIFY_U32");
                    return;
                }

                uint8_t channel = m_code[m_pc++];
                uint8_t reg = m_code[m_pc++];

                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid NOTIFY_U32 register");
                    return;
                }

                if (!m_notify_handler) {
                    vm_error("notify transport unavailable");
                    return;
                }

                uint32_t value = m_reg[reg];
                uint8_t payload[4] = {
                    (uint8_t)(value & 0xFFU),
                    (uint8_t)((value >> 8) & 0xFFU),
                    (uint8_t)((value >> 16) & 0xFFU),
                    (uint8_t)((value >> 24) & 0xFFU)
                };

                if (!m_notify_handler(NRFCLAW_VM_NOTIFY_U32,
                                      channel,
                                      payload,
                                      sizeof(payload))) {
                    vm_error("notify transport busy");
                    return;
                }

                break;
            }

            case OP_NOTIFY_STR: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated NOTIFY_STR");
                    return;
                }

                uint8_t channel = m_code[m_pc++];
                uint8_t slen = m_code[m_pc++];

                if (slen == 0U || slen > NRFCLAW_VM_MAX_NOTIFY_STRING ||
                    (uint16_t)(m_pc + slen) > m_code_len) {
                    vm_error("invalid NOTIFY_STR");
                    return;
                }

                if (!m_notify_handler) {
                    vm_error("notify transport unavailable");
                    return;
                }

                uint8_t const *s = &m_code[m_pc];
                m_pc = (uint16_t)(m_pc + slen);

                if (!m_notify_handler(NRFCLAW_VM_NOTIFY_STRING,
                                      channel,
                                      s,
                                      slen)) {
                    vm_error("notify transport busy");
                    return;
                }

                break;
            }

            case OP_STATE_SET: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated STATE_SET"); return; }
                uint8_t key=m_code[m_pc++], reg=m_code[m_pc++];
                if (key>=NRFCLAW_STATE_USER_COUNT || reg>=VM_REG_COUNT || !nrfclaw_state_set(key,m_reg[reg])) {
                    vm_error("STATE_SET rejected"); return;
                }
                break;
            }

            case OP_STATE_GET: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated STATE_GET"); return; }
                uint8_t key=m_code[m_pc++], reg=m_code[m_pc++]; uint32_t value=0U;
                if (key>=NRFCLAW_STATE_USER_COUNT || reg>=VM_REG_COUNT || !nrfclaw_state_get(key,&value)) {
                    vm_error("STATE_GET unavailable"); return;
                }
                m_reg[reg]=value;
                break;
            }

            case OP_PERSIST_SAVE: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated PERSIST_SAVE"); return; }
                uint8_t key=m_code[m_pc++], reg=m_code[m_pc++];
                if (key>=NRFCLAW_STATE_USER_COUNT || reg>=VM_REG_COUNT || !nrfclaw_state_persist(key,m_reg[reg])) {
                    vm_error("PERSIST_SAVE busy/rejected"); return;
                }
                m_reg_state_key_valid[reg] = true;
                m_reg_state_key[reg] = key;
                break;
            }

            case OP_PERSIST_LOAD: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated PERSIST_LOAD"); return; }
                uint8_t key=m_code[m_pc++], reg=m_code[m_pc++]; uint32_t value=0U;
                if (key>=NRFCLAW_STATE_USER_COUNT || reg>=VM_REG_COUNT || !nrfclaw_state_get(key,&value)) {
                    vm_error("PERSIST_LOAD unavailable"); return;
                }
                m_reg[reg]=value;
                m_reg_state_key_valid[reg] = true;
                m_reg_state_key[reg] = key;
                break;
            }

            case OP_PERSIST_LOAD_DEFAULT: {
                uint8_t key, reg;
                uint32_t value, fallback;
                if ((uint16_t)(m_pc + 6U) > m_code_len) { vm_error("truncated PERSIST_LOAD_DEFAULT"); return; }
                key = m_code[m_pc++];
                reg = m_code[m_pc++];
                fallback = (uint32_t)m_code[m_pc] |
                           ((uint32_t)m_code[m_pc + 1U] << 8) |
                           ((uint32_t)m_code[m_pc + 2U] << 16) |
                           ((uint32_t)m_code[m_pc + 3U] << 24);
                m_pc = (uint16_t)(m_pc + 4U);
                if (key >= NRFCLAW_STATE_USER_COUNT || reg >= VM_REG_COUNT) {
                    vm_error("PERSIST_LOAD_DEFAULT bad operand"); return;
                }
                if (!nrfclaw_state_get(key, &value)) {
                    value = fallback;
                    if (!nrfclaw_state_set(key, value)) {
                        vm_error("PERSIST_LOAD_DEFAULT rejected"); return;
                    }
                }
                m_reg[reg] = value;
                m_reg_state_key_valid[reg] = true;
                m_reg_state_key[reg] = key;
                break;
            }

            case OP_GPIO_WRITE_IMM: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated GPIO_WRITE_IMM"); return; }
                uint8_t pin=m_code[m_pc++];
                uint8_t value=m_code[m_pc++];
                if (nrfclaw_native_gpio_write(pin, value != 0U) != NRFCLAW_NATIVE_OK) {
                    vm_error("GPIO_WRITE_IMM rejected"); return;
                }
                break;
            }

            case OP_GPIO_WRITE_REG: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated GPIO_WRITE_REG"); return; }
                uint8_t pin=m_code[m_pc++], reg=m_code[m_pc++];
                if (reg>=VM_REG_COUNT) { vm_error("invalid GPIO_WRITE_REG register"); return; }
                if (nrfclaw_native_gpio_write(pin, m_reg[reg] != 0U) != NRFCLAW_NATIVE_OK) {
                    vm_error("GPIO_WRITE_REG rejected"); return;
                }
                break;
            }

            case OP_GPIO_READ: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated GPIO_READ"); return; }
                uint8_t pin=m_code[m_pc++], reg=m_code[m_pc++];
                if (reg>=VM_REG_COUNT) { vm_error("invalid GPIO_READ register"); return; }
                uint32_t value=0U;
                if (nrfclaw_native_gpio_read(pin, &value) != NRFCLAW_NATIVE_OK) {
                    vm_error("GPIO_READ rejected"); return;
                }
                m_reg[reg]=value;
                break;
            }

            case OP_ACCEL_READ: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) { vm_error("truncated ACCEL_READ"); return; }
                uint8_t rx=m_code[m_pc++], ry=m_code[m_pc++], rz=m_code[m_pc++];
                if (rx>=VM_REG_COUNT || ry>=VM_REG_COUNT || rz>=VM_REG_COUNT) {
                    vm_error("invalid ACCEL_READ register"); return;
                }
                int16_t x,y,z;
                if (nrfclaw_native_accel_xyz(&x,&y,&z) != NRFCLAW_NATIVE_OK) {
                    vm_error("accelerometer unavailable"); return;
                }
                m_reg[rx]=(uint32_t)(int32_t)x;
                m_reg[ry]=(uint32_t)(int32_t)y;
                m_reg[rz]=(uint32_t)(int32_t)z;
                break;
            }

            case OP_ACCEL_MOTION: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) { vm_error("truncated ACCEL_MOTION"); return; }
                uint16_t th=(uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U]<<8);
                m_pc+=2U;
                uint8_t duration=m_code[m_pc++];
                if (nrfclaw_native_accel_motion(th,duration) != NRFCLAW_NATIVE_OK) {
                    vm_error("accelerometer motion unavailable"); return;
                }
                break;
            }

            case OP_ACCEL_CONFIG: {
                if ((uint16_t)(m_pc + 9U) > m_code_len) { vm_error("truncated ACCEL_CONFIG"); return; }
                nrfclaw_accel_config_t c;
                c.mode=(nrfclaw_accel_mode_t)m_code[m_pc++];
                c.odr_hz=(uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2;
                c.full_scale_g=m_code[m_pc++];
                c.threshold_mg=(uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2;
                c.duration_ms=(uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2;
                c.low_power=(m_code[m_pc++]&1U)!=0U;
                if(nrfclaw_native_accel_configure(&c)!=NRFCLAW_NATIVE_OK){vm_error("ACCEL_CONFIG rejected");return;}
                break;
            }

            case OP_DS18_READ: {
                if (m_pc >= m_code_len) { vm_error("truncated DS18_READ"); return; }
                uint8_t reg=m_code[m_pc++];
                if(reg>=VM_REG_COUNT){vm_error("invalid DS18 register");return;}

                /*
                 * R3.8.18j: DS18 READ is an acquisition operation, not a
                 * cached-value lookup.  A program loop must start a fresh
                 * CONVERT T on every execution of this opcode.  The cached
                 * temperature API remains available to NDP and diagnostics,
                 * but must not short-circuit VM acquisition.
                 */
                if(!nrfclaw_temperature_start()){vm_error("temperature unavailable");return;}
                m_pending_ds18_reg=reg;
                m_state=NRFCLAW_VM_WAIT_DS18B20;
                return;
            }

            case OP_VIB_MODEL_SET: {
                if ((uint16_t)(m_pc + 16U) > m_code_len) { vm_error("truncated VIB_MODEL_SET"); return; }
                nrfclaw_vib_model_t model;
                model.rms_mean_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.rms_tol_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.peak_mean_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.peak_tol_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.p2p_mean_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.p2p_tol_mg=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.zero_cross_mean_hz=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                model.zero_cross_tol_hz=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                if (!nrfclaw_vib_health_model_set(&model)) {
                    vm_error("VIB model rejected"); return;
                }
                break;
            }

            case OP_VIB_SCORE: {
                if (m_pc >= m_code_len) { vm_error("truncated VIB_SCORE"); return; }
                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) { vm_error("invalid VIB_SCORE register"); return; }
                nrfclaw_vib_health_result_t result;
                if (!nrfclaw_vib_health_score(&result)) {
                    vm_error("VIB score unavailable"); return;
                }
                m_reg[reg] = result.score_q8_8;
                break;
            }

            case OP_CAP_AVAILABLE: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated CAP_AVAILABLE"); return; }
                uint8_t cap=m_code[m_pc++], reg=m_code[m_pc++];
                if (reg>=VM_REG_COUNT) { vm_error("invalid CAP_AVAILABLE register"); return; }
                m_reg[reg]=nrfclaw_native_capability_available(cap) ? 1U : 0U;
                break;
            }

            case OP_APP_GET_U32: {
                if ((uint16_t)(m_pc + 2U) > m_code_len) { vm_error("truncated APP_GET_U32"); return; }
                uint8_t offset=m_code[m_pc++], reg=m_code[m_pc++];
                if (reg>=VM_REG_COUNT) { vm_error("invalid APP_GET_U32 register"); return; }
                nrfclaw_app_frame_t frame;
                if (!nrfclaw_ble_app_last_frame(&frame) || (uint16_t)offset+4U > frame.length) {
                    vm_error("APP_GET_U32 unavailable"); return;
                }
                m_reg[reg]=read_u32_le(&frame.payload[offset]);
                break;
            }

            case OP_APP_SEND_U32: {
                if ((uint16_t)(m_pc + 5U) > m_code_len) { vm_error("truncated APP_SEND_U32"); return; }
                uint8_t type=m_code[m_pc++], cap=m_code[m_pc++], operation=m_code[m_pc++];
                uint8_t seqreg=m_code[m_pc++], valreg=m_code[m_pc++];
                if(seqreg>=VM_REG_COUNT || valreg>=VM_REG_COUNT){vm_error("invalid APP_SEND_U32 register");return;}
                nrfclaw_app_frame_t frame; memset(&frame,0,sizeof(frame));
                frame.version=NRFCLAW_APP_PROTO_VERSION; frame.type=type; frame.sequence=(uint8_t)m_reg[seqreg];
                frame.capability=cap; frame.operation=operation; frame.length=4U;
                uint32_t v=m_reg[valreg]; frame.payload[0]=(uint8_t)v;frame.payload[1]=(uint8_t)(v>>8);frame.payload[2]=(uint8_t)(v>>16);frame.payload[3]=(uint8_t)(v>>24);
                nrfclaw_ble_app_status_t st=nrfclaw_ble_app_send(&frame);
                if(st!=NRFCLAW_BLE_APP_OK){vm_error("APP_SEND_U32 unavailable/busy");return;}
                break;
            }

            case OP_SERIAL_CONFIG: {
                if ((uint16_t)(m_pc + 6U) > m_code_len) {
                    vm_error("truncated SERIAL_CONFIG");
                    return;
                }

                uint8_t tx_pin =
                    m_code[m_pc++];

                uint8_t rx_pin =
                    m_code[m_pc++];

                uint32_t baud =
                    read_u32_le(
                        &m_code[m_pc]
                    );

                m_pc +=
                    4U;


                if (nrfclaw_native_serial_enable(
                        tx_pin,
                        rx_pin,
                        baud
                    ) != NRFCLAW_NATIVE_OK)
                {
                    vm_error(
                        "SERIAL config rejected"
                    );

                    return;
                }

                break;
            }


            case OP_SERIAL_CONFIG_ECO: {
                if ((uint16_t)(m_pc + 6U) > m_code_len) {
                    vm_error("truncated SERIAL_CONFIG_ECO");
                    return;
                }
                uint8_t tx_pin = m_code[m_pc++];
                uint8_t rx_pin = m_code[m_pc++];
                uint32_t baud = read_u32_le(&m_code[m_pc]);
                m_pc += 4U;
                if (nrfclaw_native_serial_enable_economy(tx_pin, rx_pin, baud) != NRFCLAW_NATIVE_OK) {
                    vm_error("SERIAL economy config rejected");
                    return;
                }
                break;
            }

            case OP_SERIAL_DISABLE:
            {
                nrfclaw_native_serial_disable();
                break;
            }


            case OP_SERIAL_WRITE:
            {
                if (m_pc >= m_code_len)
                {
                    vm_error(
                        "truncated SERIAL_WRITE"
                    );

                    return;
                }


                uint8_t slen =
                    m_code[m_pc++];


                if (slen == 0U ||
                    slen > 64U ||
                    (uint16_t)(m_pc + slen) > m_code_len)
                {
                    vm_error(
                        "invalid SERIAL_WRITE"
                    );

                    return;
                }


                /*
                 * nrfclaw_serial/native copies the bytes into its own TX
                 * buffer. Therefore it is safe to advance PC immediately.
                 */
                if (nrfclaw_native_serial_write(
                        &m_code[m_pc],
                        slen
                    ) != NRFCLAW_NATIVE_OK)
                {
                    vm_error(
                        "SERIAL write rejected"
                    );

                    return;
                }


                m_pc =
                    (uint16_t)(
                        m_pc +
                        slen
                    );


                /*
                 * Critical Stage 8.1 fix:
                 *
                 * nrf_drv_uart_tx() is asynchronous. Do NOT execute the next
                 * opcode (often SERIAL_DISABLE) until TX_DONE arrives.
                 */
                m_state =
                    NRFCLAW_VM_WAIT_SERIAL_TX;

                return;
            }


            case OP_SERIAL_READ:
            {
                if (m_pc >= m_code_len)
                {
                    vm_error(
                        "truncated SERIAL_READ"
                    );

                    return;
                }


                uint8_t reg =
                    m_code[m_pc++];


                if (reg >= VM_REG_COUNT)
                {
                    vm_error(
                        "invalid SERIAL_READ register"
                    );

                    return;
                }


                uint32_t value =
                    0U;

                nrfclaw_native_status_t st =
                    nrfclaw_native_serial_read(
                        &value
                    );


                if (st == NRFCLAW_NATIVE_BUSY)
                {
                    vm_error(
                        "SERIAL RX empty"
                    );

                    return;
                }


                if (st != NRFCLAW_NATIVE_OK)
                {
                    vm_error(
                        "SERIAL unavailable"
                    );

                    return;
                }


                m_reg[reg] =
                    value;

                break;
            }


            case OP_TRACKING_SET_KEY:
            {
                if ((uint16_t)(m_pc + NRFCLAW_TRACKING_KEY_SIZE) > m_code_len)
                {
                    vm_error("truncated TRACKING_SET_KEY");
                    return;
                }

                nrfclaw_tracking_status_t st =
                    nrfclaw_tracking_set_manual_key(&m_code[m_pc]);

                m_pc = (uint16_t)(m_pc + NRFCLAW_TRACKING_KEY_SIZE);

                if (st != NRFCLAW_TRACKING_OK)
                {
                    vm_error("TRACKING key rejected");
                    return;
                }

                break;
            }

            case OP_TRACKING_CONFIG:
            {
                if ((uint16_t)(m_pc + 3U) > m_code_len)
                {
                    vm_error("truncated TRACKING_CONFIG");
                    return;
                }

                uint16_t interval_ms =
                    (uint16_t)m_code[m_pc] |
                    ((uint16_t)m_code[m_pc + 1U] << 8);

                m_pc += 2U;

                int8_t tx_power_dbm =
                    (int8_t)m_code[m_pc++];

                nrfclaw_tracking_info_t info;
                nrfclaw_tracking_info(&info);

                uint32_t rotation_seconds =
                    info.rotation_seconds;

                if (rotation_seconds == 0U)
                {
                    rotation_seconds =
                        NRFCLAW_TRACKING_DEFAULT_ROTATION_S;
                }

                nrfclaw_tracking_status_t st =
                    nrfclaw_tracking_config(
                        interval_ms,
                        tx_power_dbm,
                        rotation_seconds
                    );

                if (st != NRFCLAW_TRACKING_OK)
                {
                    vm_error("TRACKING config rejected");
                    return;
                }

                break;
            }

            case OP_TRACKING_START:
            {
                nrfclaw_tracking_status_t st =
                    nrfclaw_tracking_start();

                if (st == NRFCLAW_TRACKING_BUSY)
                {
                    vm_error("TRACKING busy");
                    return;
                }

                if (st == NRFCLAW_TRACKING_NO_IDENTITY)
                {
                    vm_error("TRACKING identity unavailable");
                    return;
                }

                if (st != NRFCLAW_TRACKING_OK)
                {
                    vm_error("TRACKING start failed");
                    return;
                }

                break;
            }

            case OP_TRACKING_STOP:
                nrfclaw_tracking_stop();
                break;

            case OP_TRACKING_STATUS:
            {
                if (m_pc >= m_code_len)
                {
                    vm_error("truncated TRACKING_STATUS");
                    return;
                }

                uint8_t reg = m_code[m_pc++];

                if (reg >= VM_REG_COUNT)
                {
                    vm_error("invalid TRACKING_STATUS register");
                    return;
                }

                m_reg[reg] = nrfclaw_tracking_active() ? 1U : 0U;
                break;
            }

            case OP_TRACKING_ROTATION_SET:
            {
                if ((uint16_t)(m_pc + 4U) > m_code_len)
                {
                    vm_error("truncated TRACKING_ROTATION_SET");
                    return;
                }

                uint32_t rotation_seconds =
                    ((uint32_t)m_code[m_pc]) |
                    ((uint32_t)m_code[m_pc + 1U] << 8) |
                    ((uint32_t)m_code[m_pc + 2U] << 16) |
                    ((uint32_t)m_code[m_pc + 3U] << 24);

                m_pc += 4U;

                nrfclaw_tracking_status_t st =
                    nrfclaw_tracking_rotation_set(
                        rotation_seconds
                    );

                if (st != NRFCLAW_TRACKING_OK)
                {
                    vm_error("TRACKING rotation rejected");
                    return;
                }

                break;
            }

            case OP_LORA_SEND_BAT: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated LORA_SEND_BAT");
                    return;
                }

                uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) {
                    vm_error("invalid LORA_SEND_BAT register");
                    return;
                }

                uint16_t cv = (uint16_t)m_reg[reg];
                uint8_t payload[3] = {
                    0x01,
                    (uint8_t)(cv & 0xFFU),
                    (uint8_t)(cv >> 8)
                };

                if (!nrfclaw_lora_send_async(payload, sizeof(payload))) {
                    vm_error("LoRa send rejected");
                    return;
                }

                m_state = NRFCLAW_VM_WAIT_LORA;
                return;
            }

            case OP_LORA_SEND_BYTES: {
                if (m_pc >= m_code_len) { vm_error("truncated LORA_SEND_BYTES"); return; }
                uint8_t plen=m_code[m_pc++];
                if (plen==0U || plen>32U || (uint16_t)(m_pc+plen)>m_code_len) {
                    vm_error("invalid LORA_SEND_BYTES"); return;
                }
                if (!nrfclaw_lora_send_async(&m_code[m_pc], plen)) {
                    vm_error("LoRa send rejected"); return;
                }
                m_pc=(uint16_t)(m_pc+plen);
                m_state=NRFCLAW_VM_WAIT_LORA;
                return;
            }

            case OP_LORA_CONFIG: {
                if ((uint16_t)(m_pc + 9U) > m_code_len) { vm_error("truncated LORA_CONFIG"); return; }
                nrfclaw_lora_profile_t p;
                /* OP_LORA_CONFIG wire ABI remains frozen at 9 bytes. Preserve
                 * extended sync-word/preamble fields from the active profile. */
                nrfclaw_lora_get_profile(&p);
                p.frequency_hz = read_u32_le(&m_code[m_pc]); m_pc += 4U;
                p.power_dbm = (int8_t)m_code[m_pc++];
                p.sf = m_code[m_pc++];
                p.bw_khz = (uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U] << 8); m_pc += 2U;
                p.cr = m_code[m_pc++];
                if (!nrfclaw_lora_set_profile(&p)) { vm_error("LoRa profile rejected"); return; }
                break;
            }

            case OP_FORMAT_REG: {
                if (m_pc >= m_code_len) { vm_error("truncated FORMAT_REG"); return; }
                uint8_t plen = m_code[m_pc++];
                if (plen > 48U || (uint16_t)(m_pc + plen + 2U) > m_code_len) { vm_error("invalid FORMAT_REG"); return; }
                uint8_t const *prefix = &m_code[m_pc]; m_pc = (uint16_t)(m_pc + plen);
                uint8_t reg = m_code[m_pc++]; uint8_t format = m_code[m_pc++];
                if (!vm_buffer_format_reg(prefix, plen, reg, format)) { vm_error("FORMAT_REG overflow/bad format"); return; }
                break;
            }

            case OP_FORMAT_2REG: {
                if (m_pc >= m_code_len) { vm_error("truncated FORMAT_2REG"); return; }
                uint8_t p1_len=m_code[m_pc++];
                if (p1_len>32U || (uint16_t)(m_pc+p1_len+3U)>m_code_len) { vm_error("invalid FORMAT_2REG p1"); return; }
                uint8_t const *p1=&m_code[m_pc]; m_pc=(uint16_t)(m_pc+p1_len);
                uint8_t r1=m_code[m_pc++], f1=m_code[m_pc++];
                uint8_t p2_len=m_code[m_pc++];
                if (p2_len>32U || (uint16_t)(m_pc+p2_len+2U)>m_code_len) { vm_error("invalid FORMAT_2REG p2"); return; }
                uint8_t const *p2=&m_code[m_pc]; m_pc=(uint16_t)(m_pc+p2_len);
                uint8_t r2=m_code[m_pc++], f2=m_code[m_pc++];
                if (!vm_buffer_format_2reg(p1,p1_len,r1,f1,p2,p2_len,r2,f2)) { vm_error("FORMAT_2REG overflow/bad format"); return; }
                break;
            }

            case OP_LORA_LAST_RSSI: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated LORA_LAST_RSSI");
                    return;
                }
                uint8_t reg = m_code[m_pc++];
                int16_t rssi_x2 = 0;
                int16_t snr_x4 = 0;
                if (reg >= VM_REG_COUNT ||
                    !nrfclaw_lora_get_last_packet_status(&rssi_x2, &snr_x4)) {
                    vm_error("LORA_LAST_RSSI unavailable");
                    return;
                }

                /*
                 * Driver stores RSSI as signed dBm*2. For compact beacon text,
                 * expose whole signed dBm. Sub-dB precision is intentionally
                 * discarded.
                 */
                m_reg[reg] = (uint32_t)((int32_t)rssi_x2 / 2L);
                break;
            }

            case OP_PARSE_BUF_I32: {
                if (m_pc>=m_code_len) { vm_error("truncated PARSE_BUF_I32"); return; }
                uint8_t reg=m_code[m_pc++]; int32_t v=0;
                if (reg>=VM_REG_COUNT || !vm_parse_first_signed_fixed(0U,&v)) { vm_error("PARSE_BUF_I32 failed"); return; }
                m_reg[reg]=(uint32_t)v; break;
            }

            case OP_PARSE_BUF_FIXED: {
                if ((uint16_t)(m_pc+2U)>m_code_len) { vm_error("truncated PARSE_BUF_FIXED"); return; }
                uint8_t reg=m_code[m_pc++], decimals=m_code[m_pc++]; int32_t v=0;
                if (reg>=VM_REG_COUNT || !vm_parse_first_signed_fixed(decimals,&v)) { vm_error("PARSE_BUF_FIXED failed"); return; }
                m_reg[reg]=(uint32_t)v; break;
            }

            case OP_FORMAT_REG_FIXED: {
                if (m_pc>=m_code_len) { vm_error("truncated FORMAT_REG_FIXED"); return; }
                uint8_t plen=m_code[m_pc++];
                if (plen>48U || (uint16_t)(m_pc+plen+3U)>m_code_len) { vm_error("invalid FORMAT_REG_FIXED"); return; }
                uint8_t const *prefix=&m_code[m_pc]; m_pc=(uint16_t)(m_pc+plen);
                uint8_t reg=m_code[m_pc++], decimals=m_code[m_pc++], slen=m_code[m_pc++];
                if (slen>16U || (uint16_t)(m_pc+slen)>m_code_len) { vm_error("invalid FORMAT_REG_FIXED suffix"); return; }
                uint8_t const *suffix=&m_code[m_pc]; m_pc=(uint16_t)(m_pc+slen);
                if (!vm_buffer_format_fixed(prefix,plen,reg,decimals,suffix,slen)) { vm_error("FORMAT_REG_FIXED failed"); return; }
                break;
            }

            case OP_BUFFER_PREPEND: {
                if (m_pc >= m_code_len) { vm_error("truncated BUFFER_PREPEND"); return; }
                uint8_t plen = m_code[m_pc++];
                if (plen == 0U || plen > 48U || (uint16_t)(m_pc + plen) > m_code_len) { vm_error("invalid BUFFER_PREPEND"); return; }
                if ((uint16_t)m_buffer_len + (uint16_t)plen > NRFCLAW_VM_BUFFER_SIZE) { vm_error("BUFFER_PREPEND overflow"); return; }
                memmove(&m_buffer[plen], m_buffer, m_buffer_len);
                memcpy(m_buffer, &m_code[m_pc], plen);
                m_pc = (uint16_t)(m_pc + plen);
                m_buffer_len = (uint8_t)(m_buffer_len + plen);
                break;
            }

            case OP_NINALINK_TELEMETRY_SYNTH: {
                uint16_t period_s, window_ms, backoff_ms;
                uint8_t attempts;
                int32_t temperature_mC;
                if ((uint16_t)(m_pc + 11U) > m_code_len) { vm_error("truncated NINALINK_TELEMETRY_SYNTH"); return; }
                period_s=(uint16_t)m_code[m_pc]|((uint16_t)m_code[m_pc+1U]<<8);
                window_ms=(uint16_t)m_code[m_pc+2U]|((uint16_t)m_code[m_pc+3U]<<8);
                attempts=m_code[m_pc+4U];
                backoff_ms=(uint16_t)m_code[m_pc+5U]|((uint16_t)m_code[m_pc+6U]<<8);
                temperature_mC=(int32_t)((uint32_t)m_code[m_pc+7U]|((uint32_t)m_code[m_pc+8U]<<8)|((uint32_t)m_code[m_pc+9U]<<16)|((uint32_t)m_code[m_pc+10U]<<24));
                m_pc=(uint16_t)(m_pc+11U);
                if (!nrfclaw_ninalink_telemetry_start_synthetic(period_s,window_ms,attempts,backoff_ms,temperature_mC)) { vm_error("NINALINK synthetic telemetry start rejected"); return; }
                break;
            }

            case OP_NINALINK_TELEMETRY: {
                uint16_t period_s;
                uint16_t window_ms;
                uint8_t attempts;
                uint16_t backoff_ms;

                if ((uint16_t)(m_pc + 7U) > m_code_len) {
                    vm_error("truncated NINALINK_TELEMETRY");
                    return;
                }

                period_s = (uint16_t)m_code[m_pc] |
                    ((uint16_t)m_code[m_pc + 1U] << 8);
                window_ms = (uint16_t)m_code[m_pc + 2U] |
                    ((uint16_t)m_code[m_pc + 3U] << 8);
                attempts = m_code[m_pc + 4U];
                backoff_ms = (uint16_t)m_code[m_pc + 5U] |
                    ((uint16_t)m_code[m_pc + 6U] << 8);
                m_pc = (uint16_t)(m_pc + 7U);

                if (!nrfclaw_ninalink_telemetry_start(
                        period_s,
                        window_ms,
                        attempts,
                        backoff_ms)) {
                    vm_error("NINALINK telemetry start rejected");
                    return;
                }
                break;
            }

            case OP_HA_ROLE_CONFIG: {
                if (m_pc >= m_code_len) {
                    vm_error("truncated HA_ROLE_CONFIG");
                    return;
                }
                nrfclaw_ha_role_t role = (nrfclaw_ha_role_t)m_code[m_pc++];
                if (role > NRFCLAW_HA_ROLE_BRIDGE ||
                    !nrfclaw_ha_role_configure(role)) {
                    vm_error("HA role configuration rejected/busy");
                    return;
                }
                break;
            }

            case OP_HA_NINALINK_NODE_CONFIG: {
                uint16_t period_s;
                if ((uint16_t)(m_pc + 2U) > m_code_len) {
                    vm_error("truncated HA_NINALINK_NODE_CONFIG");
                    return;
                }
                period_s = (uint16_t)m_code[m_pc] |
                    ((uint16_t)m_code[m_pc + 1U] << 8);
                m_pc = (uint16_t)(m_pc + 2U);
                if (!nrfclaw_ha_role_configure_node(period_s)) {
                    vm_error("HA NinaLink node interval rejected/busy");
                    return;
                }
                break;
            }

            case OP_LORA_PROFILE_PERSIST: {
                nrfclaw_lora_profile_t p;
                nrfclaw_lora_profile_t current;
                uint8_t mask;
                uint32_t frequency_hz;
                int8_t power_dbm;
                uint8_t sf;
                uint16_t bw_khz;
                uint8_t cr;

                if ((uint16_t)(m_pc + 10U) > m_code_len) {
                    vm_error("truncated LORA_PROFILE_PERSIST");
                    return;
                }

                mask = m_code[m_pc++];
                frequency_hz = read_u32_le(&m_code[m_pc]); m_pc += 4U;
                power_dbm = (int8_t)m_code[m_pc++];
                sf = m_code[m_pc++];
                bw_khz = (uint16_t)m_code[m_pc] |
                    ((uint16_t)m_code[m_pc + 1U] << 8); m_pc += 2U;
                cr = m_code[m_pc++];

                if (mask == 0U || (mask & 0xE0U) != 0U) {
                    vm_error("invalid LORA_PROFILE_PERSIST mask");
                    return;
                }

                nrfclaw_lora_get_profile(&current);
                p = current;
                if (mask & 0x01U) p.frequency_hz = frequency_hz;
                if (mask & 0x02U) p.power_dbm = power_dbm;
                if (mask & 0x04U) p.sf = sf;
                if (mask & 0x08U) p.bw_khz = bw_khz;
                if (mask & 0x10U) p.cr = cr;

                if (!nrfclaw_lora_profile_validate(&p)) {
                    vm_error("persistent LoRa profile invalid");
                    return;
                }

                if (memcmp(&p, &current, sizeof(p)) != 0 &&
                    !nrfclaw_lora_set_profile_persist(&p)) {
                    vm_error("persistent LoRa profile rejected/busy");
                    return;
                }
                break;
            }

            case OP_BUFFER_SET: {
                if (m_pc >= m_code_len) { vm_error("truncated BUFFER_SET"); return; }
                uint8_t blen = m_code[m_pc++];
                if (blen == 0U || blen > NRFCLAW_VM_BUFFER_SIZE || (uint16_t)(m_pc + blen) > m_code_len) { vm_error("invalid BUFFER_SET"); return; }
                memcpy(m_buffer, &m_code[m_pc], blen);
                m_pc = (uint16_t)(m_pc + blen);
                m_buffer_len = blen;
                break;
            }

            case OP_LORA_SEND_BUF:
                if (m_buffer_len == 0U || !nrfclaw_lora_send_async(m_buffer, m_buffer_len)) { vm_error("LoRa buffer send rejected"); return; }
                m_state = NRFCLAW_VM_WAIT_LORA;
                return;

            case OP_LORA_RX_START:
                if (m_pc >= m_code_len) { vm_error("truncated LORA_RX_START"); return; }
                (void)m_code[m_pc++]; /* flags reserved; current implementation captures next packet */
                if (!nrfclaw_lora_receive_async()) { vm_error("LoRa RX start rejected"); return; }
                m_state = NRFCLAW_VM_WAIT_LORA_RX;
                return;

            case OP_SERIAL_WRITE_BUF:
                if (m_buffer_len == 0U || !nrfclaw_native_serial_write(m_buffer, m_buffer_len)) { vm_error("SERIAL_WRITE_BUF rejected"); return; }
                m_state = NRFCLAW_VM_WAIT_SERIAL_TX;
                return;

            case OP_SERIAL_RX_BUF: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) { vm_error("truncated SERIAL_RX_BUF"); return; }
                uint8_t mode = m_code[m_pc++];
                uint16_t param = (uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc + 1U] << 8);
                m_pc += 2U;
                if (mode > (uint8_t)NRFCLAW_SERIAL_FRAME_LENGTH ||
                    !nrfclaw_serial_frame_arm((nrfclaw_serial_frame_mode_t)mode, param)) {
                    vm_error("SERIAL_RX_BUF arm rejected");
                    return;
                }
                m_state = NRFCLAW_VM_WAIT_SERIAL_RX;
                return;
            }

            case OP_APP_SEND_BUF: {
                if (m_buffer_len == 0U || m_buffer_len > 20U) { vm_error("APP_SEND_BUF invalid length"); return; }
                nrfclaw_ble_app_status_t st = nrfclaw_ble_app_send_raw(m_buffer, m_buffer_len);
                if (st != NRFCLAW_BLE_APP_OK) { vm_error("APP_SEND_BUF rejected/not connected"); return; }
                break;
            }

            case OP_DEBUG_BUFFER: {
                ((void)0);
                for (uint8_t i = 0U; i < m_buffer_len; ++i)
                    ((void)0);
                ((void)0);
                for (uint8_t i = 0U; i < m_buffer_len; ++i) {
                    uint8_t c = m_buffer[i];
                    ((void)0);
                }
                ((void)0);
                break;
            }

            case OP_BLE_ADV_BUF:
                if (m_buffer_len == 0U || m_buffer_len > 24U) { vm_error("BLE_ADV_BUF invalid length"); return; }
                (void)nrfclaw_ble_app_adv_stop();
                if (nrfclaw_ble_app_adv_name(m_buffer, m_buffer_len) != NRFCLAW_BLE_APP_OK ||
                    nrfclaw_ble_app_adv_start() != NRFCLAW_BLE_APP_OK) { vm_error("BLE_ADV_BUF update rejected"); return; }
                break;

            case OP_APP_EVENT_SEND: {
                if ((uint16_t)(m_pc + 3U) > m_code_len) { vm_error("truncated APP_EVENT_SEND"); return; }
                uint8_t cap = m_code[m_pc++]; uint8_t operation = m_code[m_pc++]; uint8_t reg = m_code[m_pc++];
                if (reg >= VM_REG_COUNT) { vm_error("APP_EVENT_SEND bad register"); return; }
                memset(&m_app_event_frame, 0, sizeof(m_app_event_frame));
                m_app_event_frame.version = NRFCLAW_APP_PROTO_VERSION;
                m_app_event_frame.type = NRFCLAW_APP_MSG_EVENT;
                m_app_event_frame.sequence = ++m_app_event_sequence;
                m_app_event_frame.capability = cap;
                m_app_event_frame.operation = operation;
                m_app_event_frame.length = 4U;
                m_app_event_frame.payload[0] = (uint8_t)m_reg[reg];
                m_app_event_frame.payload[1] = (uint8_t)(m_reg[reg] >> 8);
                m_app_event_frame.payload[2] = (uint8_t)(m_reg[reg] >> 16);
                m_app_event_frame.payload[3] = (uint8_t)(m_reg[reg] >> 24);
                m_app_event_pending = true;
                app_event_try_send();
                break;
            }

            case OP_BLE_APP_ROLE: {
                if (m_pc>=m_code_len) { vm_error("truncated BLE_APP_ROLE"); return; }
                uint8_t role=m_code[m_pc++];

                /*
                 * B7.6f2k3b: HA role NONE intentionally suspends the shared
                 * Application/NDP advertising plane during boot.  A standalone
                 * VM program that subsequently claims ADVERTISER/PERIPHERAL
                 * ownership must therefore resume that plane explicitly before
                 * selecting the role.  Without this, BLE_ADV_CONFIG succeeds
                 * but BLE_ADV_START/BLE_ADV_BUF is rejected as BUSY and legacy
                 * BOOT beacon programs never appear on air.
                 *
                 * Programming/P0.21 remains safe: opening NUS stops the VM and
                 * suspends its scheduler before another opcode can execute.
                 */
                if (role == (uint8_t)NRFCLAW_BLE_APP_ADVERTISER ||
                    role == (uint8_t)NRFCLAW_BLE_APP_PERIPHERAL) {
                    nrfclaw_ble_app_resume();
                }

                if (nrfclaw_ble_app_set_role((nrfclaw_ble_app_role_t)role) != NRFCLAW_BLE_APP_OK) {
                    vm_error("BLE role rejected"); return;
                }
                break;
            }

            case OP_BLE_ADV_CONFIG: {
                if ((uint16_t)(m_pc+4U)>m_code_len) { vm_error("truncated BLE_ADV_CONFIG"); return; }
                uint16_t interval=(uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc+1U]<<8); m_pc+=2U;
                int8_t tx=(int8_t)m_code[m_pc++];
                uint8_t plen=m_code[m_pc++];
                if (plen>31U || (uint16_t)(m_pc+plen)>m_code_len) { vm_error("invalid BLE_ADV_CONFIG"); return; }
                if (nrfclaw_ble_app_adv_config(interval,tx,&m_code[m_pc],plen) != NRFCLAW_BLE_APP_OK) {
                    vm_error("BLE adv config rejected"); return;
                }
                m_pc=(uint16_t)(m_pc+plen);
                break;
            }

            case OP_BLE_ADV_NAME: {
                if (m_pc >= m_code_len) { vm_error("truncated BLE_ADV_NAME"); return; }
                uint8_t nlen=m_code[m_pc++];
                if (nlen==0U || nlen>24U || (uint16_t)(m_pc+nlen)>m_code_len) { vm_error("invalid BLE_ADV_NAME"); return; }
                if (nrfclaw_ble_app_adv_name(&m_code[m_pc],nlen) != NRFCLAW_BLE_APP_OK) { vm_error("BLE adv name rejected"); return; }
                m_pc=(uint16_t)(m_pc+nlen);
                break;
            }

            case OP_BLE_ADV_START:
                if (nrfclaw_ble_app_adv_start() != NRFCLAW_BLE_APP_OK) { vm_error("BLE adv backend unavailable"); return; }
                break;

            case OP_BLE_ADV_STOP:
                if (nrfclaw_ble_app_adv_stop() != NRFCLAW_BLE_APP_OK) { vm_error("BLE adv stop rejected"); return; }
                break;

            case OP_BLE_CONNECT: {
                if ((uint16_t)(m_pc+6U)>m_code_len) { vm_error("truncated BLE_CONNECT"); return; }
                if (nrfclaw_ble_app_connect(&m_code[m_pc]) != NRFCLAW_BLE_APP_OK) { vm_error("BLE central backend unavailable"); return; }
                m_pc+=6U;
                break;
            }

            case OP_BLE_DISCONNECT:
                if (nrfclaw_ble_app_disconnect() != NRFCLAW_BLE_APP_OK) { vm_error("BLE disconnect rejected"); return; }
                break;

            case OP_VIB_AUTO_START: {
                if (m_pc >= m_code_len) { vm_error("truncated VIB_AUTO_START"); return; }
                bool relearn=(m_code[m_pc++] & 1U) != 0U;
                if (!nrfclaw_vib_auto_start(relearn)) { vm_error("VIB_AUTO start rejected"); return; }
                break;
            }

            case OP_VIB_AUTO_STOP:
                nrfclaw_vib_auto_stop();
                break;

            case OP_VIB_AUTO_CONFIG_TIME: {
                if ((uint16_t)(m_pc+8U)>m_code_len) { vm_error("truncated VIB_AUTO_CONFIG_TIME"); return; }
                uint32_t learning=(uint32_t)m_code[m_pc] | ((uint32_t)m_code[m_pc+1U]<<8) | ((uint32_t)m_code[m_pc+2U]<<16) | ((uint32_t)m_code[m_pc+3U]<<24);
                uint32_t arming=(uint32_t)m_code[m_pc+4U] | ((uint32_t)m_code[m_pc+5U]<<8) | ((uint32_t)m_code[m_pc+6U]<<16) | ((uint32_t)m_code[m_pc+7U]<<24);
                m_pc += 8U;
                nrfclaw_vib_auto_config_t vc;
                nrfclaw_vib_auto_get_config(&vc);
                vc.learning_time_s=learning; vc.arming_delay_s=arming;
                vc.confirm_delay_s=(uint8_t)(arming>255UL?255U:arming);
                if (!nrfclaw_vib_auto_set_config(&vc)) { vm_error("VIB_AUTO_CONFIG_TIME rejected"); return; }
                break;
            }

            case OP_SYSTEM_MIN_POWER:
                nrfclaw_system_minimum_power();
                break;

            case OP_HALL_CONFIG: {
                if ((uint16_t)(m_pc+5U)>m_code_len) { vm_error("truncated HALL_CONFIG"); return; }
                nrfclaw_hall_config_t hc;
                hc.mode=(nrfclaw_hall_mode_t)m_code[m_pc++];
                hc.channel=m_code[m_pc++];
                hc.pullup=m_code[m_pc++]!=0U;
                hc.count_edges=m_code[m_pc++]!=0U;
                hc.emit_events=m_code[m_pc++]!=0U;
                if (hc.mode==NRFCLAW_HALL_MODE_DISABLED) nrfclaw_hall_disable();
                else if (nrfclaw_native_hall_configure(&hc)!=NRFCLAW_NATIVE_OK) { vm_error("HALL_CONFIG rejected"); return; }
                break;
            }

            case OP_SEMANTIC_PUBLISH: {
                uint16_t cap;
                uint8_t channel, reg;
                if ((uint16_t)(m_pc + 4U) > m_code_len) { vm_error("truncated SEMANTIC_PUBLISH"); return; }
                cap = (uint16_t)m_code[m_pc] | ((uint16_t)m_code[m_pc + 1U] << 8);
                channel = m_code[m_pc + 2U];
                reg = m_code[m_pc + 3U];
                m_pc += 4U;
                if (reg >= VM_REG_COUNT) { vm_error("SEMANTIC_PUBLISH bad register"); return; }
                if (!nrfclaw_vm_semantic_state_publish(cap, channel, m_reg[reg])) {
                    vm_error("SEMANTIC_PUBLISH rejected"); return;
                }
                if (m_reg_state_key_valid[reg]) {
                    m_semantic_accumulator_bound = true;
                    m_semantic_accumulator_reg = reg;
                    m_semantic_accumulator_key = m_reg_state_key[reg];
                    m_semantic_accumulator_capability = cap;
                    m_semantic_accumulator_channel = channel;
                } else {
                    m_semantic_accumulator_bound = false;
                }
                break;
            }

            default: {
                vm_error(NULL);
                return;
            }
        }
    }
}
