#include "nrfclaw_ble.h"
#include "nrfclaw_event.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_vm.h"
#include "nrfclaw_flash.h"
#include "nrfclaw_scheduler.h"
#include "nrfclaw_schedule.h"
#include "nrfclaw_native.h"
#include "nrfclaw_tracking.h"
#include "nrfclaw_factory.h"
#include "nrfclaw_ndp.h"
#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_dfu_protocol.h"

#include "ble_nus.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_soc.h"
#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"
#include "app_timer.h"
#include "app_error.h"
#include "ble_conn_params.h"
#include "ble_advdata.h"
#include "SEGGER_RTT.h"

#include <stdio.h>
#include <string.h>

#define APP_BLE_CONN_CFG_TAG       1
#define APP_BLE_OBSERVER_PRIO      3

/* R3.8.20c2e-mac1: conservative programming-link parameters.
 *
 * CoreBluetooth (used by both Bleak on macOS and Chrome Web Bluetooth) is
 * noticeably less tolerant of a peripheral that advertises a very broad
 * 100..500 ms preferred interval with slave latency 4 while the GATT link is
 * being established.  The NUS programming plane is interactive, so favor a
 * conventional low-latency profile during connection/discovery. */
#define MIN_CONN_INTERVAL          MSEC_TO_UNITS(30, UNIT_1_25_MS)
#define MAX_CONN_INTERVAL          MSEC_TO_UNITS(50, UNIT_1_25_MS)
#define SLAVE_LATENCY              0
#define CONN_SUP_TIMEOUT           MSEC_TO_UNITS(5000, UNIT_10_MS)

#define ADV_FAST_INTERVAL          160U   /* 100 ms */
#define NRFCLAW_BLE_DEFAULT_TX_POWER_DBM 4
#define ADV_FAST_TIMEOUT           0U     /* no internal timeout; app timers own session lifetime */

/* Stage-3 NUS protocol commands. */
enum {
    CMD_HELLO         = 0x01,
    CMD_TIME_SYNC     = 0x02,

    CMD_PROGRAM_BEGIN = 0x10,
    CMD_PROGRAM_DATA  = 0x11,
    CMD_PROGRAM_END   = 0x12,
    CMD_PROGRAM_RUN   = 0x13,
    CMD_PROGRAM_STOP  = 0x14,
    CMD_STATUS        = 0x15,
    CMD_PING          = 0x16,
    CMD_PROGRAM_AUTH  = 0x17,
    CMD_PROGRAM_SCHEDULE = 0x18,
    CMD_RESET = 0x19U,
    CMD_EVENT_INJECT = 0x1AU,
    CMD_CAPABILITIES = 0x1BU,
    CMD_FACTORY_RESET = 0x1CU,
    CMD_TRACKING_IDENTITY = 0x1DU,
    CMD_TRACKING_INFO = 0x1EU,
    CMD_TRACKING_DEBUG = 0x1FU,
    CMD_DFU_ENTER = 0x20U
};

enum {
    ST_OK              = 0x00,
    ST_BAD_LENGTH      = 0x01,
    ST_BAD_STATE       = 0x02,
    ST_BAD_CRC         = 0x03,
    ST_INVALID_PROGRAM = 0x04,
    ST_RANGE           = 0x05,
    ST_BUSY            = 0x06,
    ST_NOT_LOADED      = 0x07,
    ST_INTERNAL        = 0x08,
    ST_AUTH_REQUIRED   = 0x09,
    ST_AUTH_FAILED     = 0x0A
};

/* VM asynchronous notification frame:
 * E0 TYPE CHANNEL FLAGS PAYLOAD...
 * FLAGS bit0=START, bit1=END. Payload is up to 16 bytes per NUS packet. */
#define VM_EVT_FRAME        0xE0U
#define VM_EVT_FLAG_START   0x01U
#define VM_EVT_FLAG_END     0x02U
#define VM_EVT_CHUNK_MAX    16U

typedef struct {
    bool active;
    nrfclaw_vm_notify_type_t type;
    uint8_t channel;
    uint16_t len;
    uint16_t offset;
    uint8_t data[NRFCLAW_VM_MAX_NOTIFY_STRING];
} vm_notify_pending_t;

static vm_notify_pending_t m_vm_notify;
static bool m_vm_notify_wait_hvn_complete;
static bool m_run_after_hvn_complete;
static bool m_reset_after_hvn_complete;
static bool m_factory_reset_after_hvn_complete;
static bool m_dfu_after_hvn_complete;

BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);
NRF_BLE_GATT_DEF(m_gatt);
NRF_BLE_QWR_DEF(m_qwr);
static uint8_t m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;

APP_TIMER_DEF(m_discovery_timer);
APP_TIMER_DEF(m_idle_timer);

static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static bool m_programming_allowed;
static bool m_session_connected_once;
static char m_device_name_ndp[20];
static char m_device_name_nus[20];

/*
 * Shared advertising buffers used when the single SoftDevice advertising
 * handle is switched back from Application BLE to NUS programming.
 * Legacy S132 advertising/scan-response payloads are limited to 31 bytes,
 * but BLE_GAP_ADV_SET_DATA_SIZE_MAX keeps this declaration SDK-correct.
 */
static uint8_t m_prog_adv_encoded[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_prog_scan_encoded[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_mac_suffix[3];

static ret_code_t programming_adv_configure(bool start_now);
static ret_code_t programming_adv_start(void);

static void log_ret(char const *name, ret_code_t err)
{
    char msg[80];
    snprintf(msg, sizeof(msg), "BLE: %s=0x%08lX\r\n",
             name, (unsigned long)err);
    SEGGER_RTT_WriteString(0, msg);
}

static void advertising_idle(void)
{
    /*
     * There is only one advertising set in Stage 8.1.
     * "Idle" means explicitly stopped; no SDK advertising state machine is
     * allowed to restart it behind the Application plane.
     */
    if (m_adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET)
        return;

    ret_code_t err =
        sd_ble_gap_adv_stop(m_adv_handle);

    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE)
    {
        log_ret("advertising_idle", err);
    }
}

static void stop_timer_safely(app_timer_id_t timer_id)
{
    ret_code_t err = app_timer_stop(timer_id);
    if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
        log_ret("timer_stop", err);
}

static void start_idle_timer(void)
{
    stop_timer_safely(m_idle_timer);

    ret_code_t err = app_timer_start(
        m_idle_timer,
        APP_TIMER_TICKS(NRFCLAW_BLE_SESSION_IDLE_TIMEOUT_S * 1000UL),
        NULL);

    if (err != NRF_SUCCESS)
        log_ret("idle_timer_start", err);
}

static void touch_session(void)
{
    /*
     * Every valid NUS command refreshes the 5-minute inactivity timer.
     * NUS RX only exists while connected, so no extra connection test is
     * required here.
     */
    if (m_programming_allowed && m_session_connected_once)
        start_idle_timer();
}
/*
static void discovery_timeout(void *p_context)
{
    (void)p_context;

    if (m_conn_handle == BLE_CONN_HANDLE_INVALID &&
        !m_session_connected_once) {
        m_programming_allowed = false;
        advertising_idle();

        SEGGER_RTT_WriteString(
            0,
            "BLE: no connection in 60 s, programming OFF"
        );
    }
}
*/
static void discovery_timeout(void *p_context)
{
    (void)p_context;

    /*
     * Se houve conexão, este timer já deveria ter sido cancelado.
     */
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        return;
    }

    /*
     * Ninguém conectou durante a janela de programação.
     */
    m_programming_allowed = false;
    m_session_connected_once = false;

    /*
     * Não precisamos mais do timer de sessão.
     */
    stop_timer_safely(
        m_idle_timer
    );

    /*
     * Limpa qualquer estado de TX/NUS pendente.
     */
    m_vm_notify.active = false;
    m_vm_notify_wait_hvn_complete = false;
    m_run_after_hvn_complete = false;
    m_reset_after_hvn_complete = false;
    m_factory_reset_after_hvn_complete = false;
    m_dfu_after_hvn_complete = false;

    /*
     * Para explicitamente o advertising.
     */
    ret_code_t err =
        sd_ble_gap_adv_stop(
            m_adv_handle
        );

    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE)
    {
        /*
         * Sem RTT neste teste.
         * Apenas ignoramos o erro não crítico.
         */
    }

    /* Return ownership of the shared advertising set to the Application/HA
     * plane after an unused physical programming window expires. */
    nrfclaw_ble_set_gap_name_ndp();
    nrfclaw_ble_app_resume();
    if (nrfclaw_ble_app_role() == NRFCLAW_BLE_APP_PERIPHERAL ||
        nrfclaw_ble_app_role() == NRFCLAW_BLE_APP_ADVERTISER)
        (void)nrfclaw_ble_app_adv_start();
}




static void idle_timeout(void *p_context)
{
    (void)p_context;

    SEGGER_RTT_WriteString(0, "BLE: programming session timeout\r\n");
    nrfclaw_ble_programming_close();
}

static uint8_t map_upload_status(nrfclaw_upload_result_t r)
{
    switch (r) {
        case NRFCLAW_UPLOAD_OK:              return ST_OK;
        case NRFCLAW_UPLOAD_BUSY:            return ST_BUSY;
        case NRFCLAW_UPLOAD_SIZE:            return ST_RANGE;
        case NRFCLAW_UPLOAD_OFFSET:          return ST_RANGE;
        case NRFCLAW_UPLOAD_INCOMPLETE:      return ST_BAD_LENGTH;
        case NRFCLAW_UPLOAD_CRC:             return ST_BAD_CRC;
        case NRFCLAW_UPLOAD_INVALID_PROGRAM: return ST_INVALID_PROGRAM;
        case NRFCLAW_UPLOAD_AUTH_REQUIRED:   return ST_AUTH_REQUIRED;
        case NRFCLAW_UPLOAD_AUTH_FAILED:     return ST_AUTH_FAILED;
        default:                             return ST_INTERNAL;
    }
}

static bool vm_can_run_loaded(void)
{
    nrfclaw_vm_state_t state = nrfclaw_vm_state();

    if (nrfclaw_vm_loaded_program_len() == 0U)
        return false;

    return state == NRFCLAW_VM_STOPPED ||
           state == NRFCLAW_VM_DONE ||
           state == NRFCLAW_VM_ERROR;
}

static bool vm_notify_enqueue(nrfclaw_vm_notify_type_t type,
                              uint8_t channel,
                              uint8_t const *data,
                              uint16_t len)
{
    if (!data || len == 0U)
        return false;

    /*
     * NOTIFY_* is an observability/output primitive, not a requirement for
     * scheduled execution. If no BLE central is connected, drop the report
     * and let the VM continue instead of turning an autonomous task into
     * VM_ERROR.
     */
    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
        return true;

    if (m_vm_notify.active)
        return false;

    if (type == NRFCLAW_VM_NOTIFY_U32) {
        if (len != 4U)
            return false;
    }
    else if (type == NRFCLAW_VM_NOTIFY_STRING) {
        if (len > NRFCLAW_VM_MAX_NOTIFY_STRING)
            return false;
    }
    else {
        return false;
    }

    m_vm_notify.type = type;
    m_vm_notify.channel = channel;
    m_vm_notify.len = len;
    m_vm_notify.offset = 0U;
    memcpy(m_vm_notify.data, data, len);
    m_vm_notify.active = true;
    m_vm_notify_wait_hvn_complete = false;

    SEGGER_RTT_WriteString(0, "BLE: VM notify queued\r\n");
    return true;
}

static void vm_notify_process(void)
{
    if (!m_vm_notify.active ||
        m_conn_handle == BLE_CONN_HANDLE_INVALID ||
        m_vm_notify_wait_hvn_complete)
        return;

    uint16_t remaining =
        (uint16_t)(m_vm_notify.len - m_vm_notify.offset);

    uint8_t chunk =
        (remaining > VM_EVT_CHUNK_MAX) ?
        VM_EVT_CHUNK_MAX :
        (uint8_t)remaining;

    uint8_t frame[20];
    uint8_t flags = 0U;

    if (m_vm_notify.offset == 0U)
        flags |= VM_EVT_FLAG_START;

    if (chunk == remaining)
        flags |= VM_EVT_FLAG_END;

    frame[0] = VM_EVT_FRAME;
    frame[1] = (uint8_t)m_vm_notify.type;
    frame[2] = m_vm_notify.channel;
    frame[3] = flags;

    memcpy(
        &frame[4],
        &m_vm_notify.data[m_vm_notify.offset],
        chunk
    );

    uint16_t tx_len =
        (uint16_t)(4U + chunk);

    ret_code_t err =
        ble_nus_data_send(
            &m_nus,
            frame,
            &tx_len,
            m_conn_handle
        );

    if (err == NRF_SUCCESS)
    {
        m_vm_notify.offset =
            (uint16_t)(m_vm_notify.offset + chunk);

        SEGGER_RTT_WriteString(
            0,
            "BLE: VM notify sent"
        );

        if (m_vm_notify.offset >= m_vm_notify.len)
        {
            m_vm_notify.active = false;
            m_vm_notify_wait_hvn_complete = false;
            m_run_after_hvn_complete = false;
            m_vm_notify_wait_hvn_complete = false;
        }

        return;
    }

    /*
     * The RUN response and the first VM event use the same NUS TX
     * characteristic. With a small SoftDevice HVN queue, NOTIFY_U32 can
     * legitimately arrive while the RUN response is still pending.
     *
     * Do not spin and do not drop the VM event. Wait explicitly for
     * BLE_GATTS_EVT_HVN_TX_COMPLETE before trying again.
     */
    if (err == NRF_ERROR_RESOURCES ||
        err == NRF_ERROR_BUSY)
    {
        m_vm_notify_wait_hvn_complete = true;

        SEGGER_RTT_WriteString(
            0,
            "BLE: VM notify waiting HVN_TX_COMPLETE"
        );

        return;
    }

    if (err == NRF_ERROR_INVALID_STATE ||
        err == NRF_ERROR_NOT_FOUND)
    {
        SEGGER_RTT_WriteString(
            0,
            "BLE: VM notify dropped, notifications unavailable"
        );

        m_vm_notify.active = false;
        m_vm_notify_wait_hvn_complete = false;
        return;
    }

    log_ret("vm_notify", err);

    m_vm_notify.active = false;
    m_vm_notify_wait_hvn_complete = false;
}

static void nus_reply(uint8_t command, uint8_t status,
                      uint8_t const *payload, uint8_t payload_len)
{
    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
        return;

    uint8_t frame[20];
    uint16_t len = (uint16_t)(2U + payload_len);

    if (len > sizeof(frame))
        return;

    frame[0] = (uint8_t)(command | 0x80U);
    frame[1] = status;

    if (payload_len && payload)
        memcpy(&frame[2], payload, payload_len);

    ret_code_t err = ble_nus_data_send(&m_nus, frame, &len, m_conn_handle);

    /* Notifications may not yet be enabled when a central first connects.
     * That is not fatal to the runtime. */
    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE &&
        err != NRF_ERROR_RESOURCES &&
        err != NRF_ERROR_NOT_FOUND)
        log_ret("nus_reply", err);
}

static void command_hello(void)
{
    uint16_t loaded = nrfclaw_vm_loaded_program_len();

    uint8_t p[11] = {
        8U, 0U, /* protocol 8.0 */
        m_mac_suffix[0],
        m_mac_suffix[1],
        m_mac_suffix[2],
        (uint8_t)(NRFCLAW_VM_MAX_PROGRAM_SIZE & 0xFFU),
        (uint8_t)(NRFCLAW_VM_MAX_PROGRAM_SIZE >> 8),
        (uint8_t)(loaded & 0xFFU),
        (uint8_t)(loaded >> 8),
        (uint8_t)nrfclaw_vm_state(),
        0U
    };

    nus_reply(CMD_HELLO, ST_OK, p, sizeof(p));
}

static void command_status(void)
{
    uint16_t pc = nrfclaw_vm_pc();
    uint16_t loaded = nrfclaw_vm_loaded_program_len();
    uint32_t generation = nrfclaw_flash_generation();
    uint32_t next_epoch = nrfclaw_scheduler_next_epoch();
    nrfclaw_schedule_t schedule = nrfclaw_scheduler_rule();

    uint8_t p[18] = {
        (uint8_t)nrfclaw_vm_state(),
        (uint8_t)(pc & 0xFFU),
        (uint8_t)(pc >> 8),
        (uint8_t)(loaded & 0xFFU),
        (uint8_t)(loaded >> 8),
        m_programming_allowed ? 1U : 0U,

        (uint8_t)nrfclaw_flash_status(),
        nrfclaw_flash_active_slot(),

        (uint8_t)(generation & 0xFFU),
        (uint8_t)((generation >> 8) & 0xFFU),
        (uint8_t)((generation >> 16) & 0xFFU),
        (uint8_t)((generation >> 24) & 0xFFU),

        (uint8_t)nrfclaw_scheduler_state(),
        schedule.mode,

        (uint8_t)(next_epoch & 0xFFU),
        (uint8_t)((next_epoch >> 8) & 0xFFU),
        (uint8_t)((next_epoch >> 16) & 0xFFU),
        (uint8_t)((next_epoch >> 24) & 0xFFU)
    };

    nus_reply(CMD_STATUS, ST_OK, p, sizeof(p));
}

static void handle_binary_command(uint8_t const *d, uint16_t len)
{
    if (!d || len == 0U)
        return;

    uint8_t cmd = d[0];

    switch (cmd) {
        case CMD_HELLO:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }
            command_hello();
            return;

        case CMD_TIME_SYNC:
            if (len != 5U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint32_t epoch = ((uint32_t)d[1]) |
                                 ((uint32_t)d[2] << 8) |
                                 ((uint32_t)d[3] << 16) |
                                 ((uint32_t)d[4] << 24);
                nrfclaw_rtc_set_epoch(epoch);
                nrfclaw_scheduler_on_time_sync();
                nus_reply(cmd, ST_OK, NULL, 0);
                return;
            }

        case CMD_PROGRAM_BEGIN:
            if (len != 5U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint16_t total = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
                uint16_t crc   = (uint16_t)d[3] | ((uint16_t)d[4] << 8);

                /*
                 * PROGRAM_BEGIN is the exact point where debug/inspection
                 * becomes reprogramming. A VM deliberately preserved in
                 * WAIT_EVENT by P0.21 must now be stopped before replacing
                 * its RAM program slot.
                 */
                (void)nrfclaw_vm_stop();

                nrfclaw_upload_result_t r =
                    nrfclaw_vm_upload_begin(total, crc);
                nus_reply(cmd, map_upload_status(r), NULL, 0);
                return;
            }

        case CMD_PROGRAM_DATA:
            if (len < 4U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint16_t offset = (uint16_t)d[1] | ((uint16_t)d[2] << 8);
                nrfclaw_upload_result_t r =
                    nrfclaw_vm_upload_write(offset, &d[3],
                                             (uint16_t)(len - 3U));
                nus_reply(cmd, map_upload_status(r), NULL, 0);
                return;
            }

        case CMD_PROGRAM_SCHEDULE:
            if (len != (1U + NRFCLAW_SCHEDULE_WIRE_SIZE)) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                nrfclaw_schedule_t schedule;
                nrfclaw_schedule_decode(&schedule, &d[1]);

                nrfclaw_upload_result_t r =
                    nrfclaw_vm_upload_schedule(&schedule);

                nus_reply(cmd, map_upload_status(r), NULL, 0);
                return;
            }

        case CMD_PROGRAM_AUTH:
            if (len < 4U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint16_t offset =
                    (uint16_t)d[1] |
                    ((uint16_t)d[2] << 8);

                nrfclaw_upload_result_t r =
                    nrfclaw_vm_upload_auth_write(
                        offset,
                        &d[3],
                        (uint16_t)(len - 3U));

                nus_reply(
                    cmd,
                    map_upload_status(r),
                    NULL,
                    0
                );

                return;
            }

        case CMD_PROGRAM_END:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                nrfclaw_upload_result_t r = nrfclaw_vm_upload_finish();
                nus_reply(cmd, map_upload_status(r), NULL, 0);
                return;
            }

        case CMD_PROGRAM_RUN:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }

            if (nrfclaw_vm_loaded_program_len() == 0U) {
                nus_reply(cmd, ST_NOT_LOADED, NULL, 0);
                return;
            }

            if (!vm_can_run_loaded()) {
                nus_reply(cmd, ST_BUSY, NULL, 0);
                return;
            }

            /*
             * IMPORTANT:
             *
             * Do not start the VM before the RUN ACK has actually left the
             * SoftDevice HVN queue. Otherwise an immediate NOTIFY_U32/STR can
             * contend with the RUN response on the same NUS TX characteristic.
             *
             * The client first receives "RUN accepted". The corresponding
             * BLE_GATTS_EVT_HVN_TX_COMPLETE then starts the VM.
             */
            nrfclaw_scheduler_pause_for_manual_run();
            m_run_after_hvn_complete = true;

            nus_reply(cmd, ST_OK, NULL, 0);

            SEGGER_RTT_WriteString(
                0,
                "BLE: RUN ACK queued, VM waits HVN_TX_COMPLETE\r\n"
            );

            return;

        case CMD_PROGRAM_STOP:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }
            nus_reply(cmd,
                      nrfclaw_vm_stop() ? ST_OK : ST_BAD_STATE,
                      NULL, 0);
            return;

        case CMD_STATUS:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }
            command_status();
            return;

        case CMD_PING:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }
            nus_reply(cmd, ST_OK, NULL, 0);
            return;

        case CMD_RESET:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            }
            m_reset_after_hvn_complete = true;
            nus_reply(cmd, ST_OK, NULL, 0);
            return;

        case CMD_FACTORY_RESET:
            if (len != 1U) { nus_reply(cmd, ST_BAD_LENGTH, NULL, 0); return; }
            m_factory_reset_after_hvn_complete = true;
            nus_reply(cmd, ST_OK, NULL, 0);
            return;

        case CMD_DFU_ENTER:
            if (len != 1U) { nus_reply(cmd, ST_BAD_LENGTH, NULL, 0); return; }
            m_dfu_after_hvn_complete = true;
            nus_reply(cmd, ST_OK, NULL, 0);
            return;

        case CMD_TRACKING_IDENTITY:
            /*
             * Request: [cmd, offset], offset = 0 or 16.
             * Reply payload: [offset, 16 seed bytes].
             *
             * NUS is only available after physical P0.21, so identity export
             * keeps the same physical-access assumption as the rest of the
             * programming plane.
             */
            if (len != 2U || (d[1] != 0U && d[1] != 16U)) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint8_t seed[NRFCLAW_TRACKING_SEED_SIZE];
                uint8_t p[17];

                if (!nrfclaw_tracking_identity_read(seed)) {
                    nus_reply(cmd, ST_NOT_LOADED, NULL, 0);
                    return;
                }

                p[0] = d[1];
                memcpy(&p[1], &seed[d[1]], 16U);
                memset(seed, 0, sizeof(seed));

                nus_reply(cmd, ST_OK, p, sizeof(p));
                return;
            }

        case CMD_TRACKING_INFO:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                nrfclaw_tracking_info_t info;
                nrfclaw_tracking_info(&info);

                uint8_t p[16] = {
                    info.identity_valid ? 1U : 0U,
                    info.active ? 1U : 0U,
                    info.active_slot,

                    (uint8_t)(info.key_index),
                    (uint8_t)(info.key_index >> 8),
                    (uint8_t)(info.key_index >> 16),
                    (uint8_t)(info.key_index >> 24),

                    (uint8_t)(info.rotation_seconds),
                    (uint8_t)(info.rotation_seconds >> 8),
                    (uint8_t)(info.rotation_seconds >> 16),
                    (uint8_t)(info.rotation_seconds >> 24),

                    (uint8_t)(info.adv_interval_ms),
                    (uint8_t)(info.adv_interval_ms >> 8),
                    (uint8_t)info.tx_power_dbm,

                    (uint8_t)(info.generation),
                    (uint8_t)(info.generation >> 8)
                };

                nus_reply(cmd, ST_OK, p, sizeof(p));
                return;
            }

        case CMD_TRACKING_DEBUG:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                nrfclaw_tracking_debug_t dbg;
                nrfclaw_tracking_debug(&dbg);

                uint8_t p[16] = {
                    dbg.stage,
                    dbg.start_pending,
                    dbg.index_commit_pending,
                    dbg.apply_pending,
                    dbg.active,
                    dbg.commit_state,
                    dbg.journal_state,
                    0U,
                    (uint8_t)(dbg.key_index),
                    (uint8_t)(dbg.key_index >> 8),
                    (uint8_t)(dbg.key_index >> 16),
                    (uint8_t)(dbg.key_index >> 24),
                    (uint8_t)(dbg.last_sd_error),
                    (uint8_t)(dbg.last_sd_error >> 8),
                    (uint8_t)(dbg.last_sd_error >> 16),
                    (uint8_t)(dbg.last_sd_error >> 24)
                };

                nus_reply(cmd, ST_OK, p, sizeof(p));
                return;
            }

        case CMD_EVENT_INJECT:
            if (len != 10U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                nrfclaw_event_t ev;
                ev.type = (nrfclaw_event_type_t)d[1];
                ev.arg0 = ((uint32_t)d[2]) | ((uint32_t)d[3] << 8) |
                          ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
                ev.arg1 = ((uint32_t)d[6]) | ((uint32_t)d[7] << 8) |
                          ((uint32_t)d[8] << 16) | ((uint32_t)d[9] << 24);
                if (!nrfclaw_event_push(&ev)) {
                    nus_reply(cmd, ST_BUSY, NULL, 0);
                    return;
                }
                nus_reply(cmd, ST_OK, NULL, 0);
                return;
            }

        case CMD_CAPABILITIES:
            if (len != 1U) {
                nus_reply(cmd, ST_BAD_LENGTH, NULL, 0);
                return;
            } else {
                uint32_t supported = 0U;
                uint32_t active = 0U;

                for (uint8_t cap = 1U; cap <= NRFCLAW_CAP_VIB_AUTO; cap++) {
                    if (nrfclaw_native_capability_supported(cap))
                        supported |= (1UL << cap);
                    if (nrfclaw_native_capability_available(cap))
                        active |= (1UL << cap);
                }

                /* Stage 7.1: supported bitmap + currently active bitmap. */
                uint8_t p[8] = {
                    (uint8_t)(supported & 0xFFU),
                    (uint8_t)((supported >> 8) & 0xFFU),
                    (uint8_t)((supported >> 16) & 0xFFU),
                    (uint8_t)((supported >> 24) & 0xFFU),
                    (uint8_t)(active & 0xFFU),
                    (uint8_t)((active >> 8) & 0xFFU),
                    (uint8_t)((active >> 16) & 0xFFU),
                    (uint8_t)((active >> 24) & 0xFFU)
                };
                nus_reply(cmd, ST_OK, p, sizeof(p));
                return;
            }

        default:
            nus_reply(cmd, ST_BAD_STATE, NULL, 0);
            return;
    }
}

static void nus_evt(ble_nus_evt_t *p_evt)
{
    if (p_evt->type != BLE_NUS_EVT_RX_DATA)
        return;

    if (!m_programming_allowed)
        return;

    touch_session();

    {
        uint8_t const *rx = p_evt->params.rx_data.p_data;
        uint16_t rx_len = p_evt->params.rx_data.length;

        /* Stage 10.2.3: NDP-SESSION coexists with the validated legacy NUS
         * protocol. NDP frames are recognized structurally; otherwise the
         * exact legacy parser remains in control. */
        if (nrfclaw_ndp_is_session_frame(rx, rx_len)) {
            uint8_t response[NRFCLAW_NDP_MAX_FRAME];
            uint16_t response_len = 0;
            if (nrfclaw_ndp_handle_session_transport(rx, rx_len,
                                                     response, &response_len,
                                                     false)) {
                uint16_t send_len = response_len;
                ret_code_t err = ble_nus_data_send(&m_nus, response,
                                                   &send_len, m_conn_handle);
                if (err != NRF_SUCCESS &&
                    err != NRF_ERROR_RESOURCES &&
                    err != NRF_ERROR_INVALID_STATE) {
                    log_ret("ndp_send", err);
                }
                touch_session();
            }
            return;
        }

        handle_binary_command(rx, rx_len);
    }
}

static void ble_evt(ble_evt_t const *p_ble_evt, void *p_context)
{
    (void)p_context;

    switch (p_ble_evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED: {
            /*
             * This observer belongs only to the NUS programming plane.
             * When programming is closed, a connection may legitimately
             * belong to nrfclaw_ble_app (ESP32/RPi/application peripheral).
             * Do not claim or disconnect that link here.
             */
            if (!m_programming_allowed)
                break;

            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;

            APP_ERROR_CHECK(
                nrf_ble_qwr_conn_handle_assign(&m_qwr, m_conn_handle));

            /*
             * First successful connection:
             * - cancel the 60-second discovery timer;
             * - start the 5-minute inactivity timer.
             *
             * A reconnect during the same session does not reset the 5-minute
             * timer by itself. Only a valid NUS command does that.
             */
            if (!m_session_connected_once) {
                m_session_connected_once = true;
                stop_timer_safely(m_discovery_timer);
                start_idle_timer();
            }

            SEGGER_RTT_printf(
                0,
                "BLE MAC-COMPAT: CONNECTED handle=%u interval=%u latency=%u timeout=%u\r\n",
                (unsigned)m_conn_handle,
                (unsigned)p_ble_evt->evt.gap_evt.params.connected.conn_params.min_conn_interval,
                (unsigned)p_ble_evt->evt.gap_evt.params.connected.conn_params.slave_latency,
                (unsigned)p_ble_evt->evt.gap_evt.params.connected.conn_params.conn_sup_timeout
            );
            SEGGER_RTT_WriteString(
                0,
                "BLE: connected, 5-minute command timer active\r\n"
            );

            nrfclaw_event_t ev = {
                .type = NRFCLAW_EVT_BLE_CONNECTED
            };

            (void)nrfclaw_event_push(&ev);
            break;
        }

        case BLE_GAP_EVT_DISCONNECTED: {
            /* R3.7: NDP authorization state is reset on disconnect, but NUS itself
             * is intentionally keyless; physical P0.21 access is its trust boundary. */
            nrfclaw_ndp_access_on_disconnect();
            /* Ignore Application-BLE disconnects not owned by NUS. */
            if (p_ble_evt->evt.gap_evt.conn_handle != m_conn_handle)
                break;

            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            m_vm_notify.active = false;

            SEGGER_RTT_printf(
                0,
                "BLE MAC-COMPAT: DISCONNECTED handle=%u reason=0x%02X\r\n",
                (unsigned)p_ble_evt->evt.gap_evt.conn_handle,
                (unsigned)p_ble_evt->evt.gap_evt.params.disconnected.reason
            );

            /*
             * If a programming session is still authorized, immediately
             * return to FAST advertising. The 5-minute inactivity timer keeps
             * running from the last valid NUS command.
             *
             * This allows successive CLI invocations to reconnect quickly
             * without another P0.21 press.
             */
            if (m_programming_allowed && m_session_connected_once) {
                ret_code_t err =
                    programming_adv_start();

                if (err != NRF_SUCCESS &&
                    err != NRF_ERROR_INVALID_STATE)
                {
                    log_ret(
                        "restart_fast_advertising",
                        err
                    );
                }

                SEGGER_RTT_WriteString(
                    0,
                    "BLE: session open, FAST advertising"
                );
            }
            else {
                advertising_idle();
                SEGGER_RTT_WriteString(0, "BLE: programming OFF");

                /* Programming ownership ended: restore the native
                 * Application/HA peripheral advertisement. */
                nrfclaw_ble_set_gap_name_ndp();
                nrfclaw_ble_app_resume();
                if (nrfclaw_ble_app_role() == NRFCLAW_BLE_APP_PERIPHERAL ||
                    nrfclaw_ble_app_role() == NRFCLAW_BLE_APP_ADVERTISER)
                    (void)nrfclaw_ble_app_adv_start();
            }

            nrfclaw_event_t ev = {
                .type = NRFCLAW_EVT_BLE_DISCONNECTED
            };

            (void)nrfclaw_event_push(&ev);
            break;
        }

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST: {
            /* NUS is intentionally keyless after physical P0.21 access.
             * Explicitly terminate an unsolicited pairing procedure instead
             * of leaving CoreBluetooth waiting for a security reply. */
            uint16_t handle = p_ble_evt->evt.gap_evt.conn_handle;
            ret_code_t err = sd_ble_gap_sec_params_reply(
                handle,
                BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
                NULL,
                NULL);
            if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
                log_ret("sec_params_reply", err);
            SEGGER_RTT_WriteString(0, "BLE MAC-COMPAT: pairing request rejected (NUS is open)\r\n");
            break;
        }

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            /* Keep the programming plane on the universally supported 1M PHY
             * during link establishment/service discovery. */
            ble_gap_phys_t phys = {
                .tx_phys = BLE_GAP_PHY_1MBPS,
                .rx_phys = BLE_GAP_PHY_1MBPS
            };
            ret_code_t err = sd_ble_gap_phy_update(
                p_ble_evt->evt.gap_evt.conn_handle,
                &phys);
            if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
                log_ret("phy_update", err);
            SEGGER_RTT_WriteString(0, "BLE MAC-COMPAT: PHY request -> 1M/1M\r\n");
            break;
        }

        case BLE_GAP_EVT_CONN_PARAM_UPDATE:
            SEGGER_RTT_printf(
                0,
                "BLE MAC-COMPAT: CONN_PARAM_UPDATE interval=%u latency=%u timeout=%u\r\n",
                (unsigned)p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.min_conn_interval,
                (unsigned)p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.slave_latency,
                (unsigned)p_ble_evt->evt.gap_evt.params.conn_param_update.conn_params.conn_sup_timeout
            );
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING: {
            /* A fresh/non-bonded CoreBluetooth client can ask for system
             * attributes before enabling NUS notifications.  There are no
             * bonded attributes on the physical programming plane, so clear
             * them explicitly and let CCCD setup continue. */
            uint16_t handle = p_ble_evt->evt.gatts_evt.conn_handle;
            ret_code_t err = sd_ble_gatts_sys_attr_set(handle, NULL, 0, 0);
            if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
                log_ret("sys_attr_set", err);
            SEGGER_RTT_WriteString(0, "BLE MAC-COMPAT: SYS_ATTR_MISSING cleared\r\n");
            break;
        }

        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            if (m_factory_reset_after_hvn_complete)
            {
                m_factory_reset_after_hvn_complete = false;
                (void)nrfclaw_vm_stop();
                if (!nrfclaw_factory_request())
                    SEGGER_RTT_WriteString(0,"BLE ERROR: factory reset request rejected\r\n");
                break;
            }

            if (m_dfu_after_hvn_complete)
            {
                m_dfu_after_hvn_complete = false;
                (void)sd_power_gpregret_clr(0, 0xFFU);
                (void)sd_power_gpregret_set(0, NRFCLAW_DFU_GPREGRET_MAGIC);
                sd_nvic_SystemReset();
                break;
            }

            if (m_reset_after_hvn_complete)
            {
                m_reset_after_hvn_complete = false;
                sd_nvic_SystemReset();
                break;
            }

            /*
             * PROGRAM_RUN is intentionally started only after its ACK
             * notification was actually transmitted.
             */
            if (m_run_after_hvn_complete)
            {
                m_run_after_hvn_complete = false;

                if (nrfclaw_vm_run_loaded())
                {
                    SEGGER_RTT_WriteString(
                        0,
                        "BLE: RUN ACK complete -> VM started\r\n"
                    );
                }
                else
                {
                    SEGGER_RTT_WriteString(
                        0,
                        "BLE ERROR: deferred VM RUN rejected\r\n"
                    );
                }
            }

            /*
             * A VM notification can also have been deferred because the
             * SoftDevice had no HVN resources. Release it now.
             */
            if (m_vm_notify_wait_hvn_complete)
            {
                m_vm_notify_wait_hvn_complete = false;

                SEGGER_RTT_WriteString(
                    0,
                    "BLE: HVN_TX_COMPLETE -> VM notify retry\r\n"
                );
            }
            break;

        default:
            break;
    }
}

NRF_SDH_BLE_OBSERVER(m_ble_observer,
                     APP_BLE_OBSERVER_PRIO,
                     ble_evt,
                     NULL);

static void gap_set_name(char const *name)
{
    ble_gap_conn_sec_mode_t sec_mode;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    APP_ERROR_CHECK(
        sd_ble_gap_device_name_set(&sec_mode,
                                   (uint8_t const *)name,
                                   (uint16_t)strlen(name)));
}

void nrfclaw_ble_set_gap_name_ndp(void)
{
    gap_set_name(m_device_name_ndp);
}

void nrfclaw_ble_set_gap_name_nus(void)
{
    gap_set_name(m_device_name_nus);
}

void nrfclaw_ble_init(void)
{
    APP_ERROR_CHECK(nrf_sdh_enable_request());

#if defined(NRFCLAW_DCDC_ENABLE) && NRFCLAW_DCDC_ENABLE
    APP_ERROR_CHECK(sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE));
#else
    APP_ERROR_CHECK(sd_power_dcdc_mode_set(NRF_POWER_DCDC_DISABLE));
#endif

    uint32_t ram_start = 0;
    APP_ERROR_CHECK(
        nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start));
    APP_ERROR_CHECK(nrf_sdh_ble_enable(&ram_start));

    APP_ERROR_CHECK(nrf_ble_gatt_init(&m_gatt, NULL));

    nrf_ble_qwr_init_t qwr_init = {0};
    APP_ERROR_CHECK(nrf_ble_qwr_init(&m_qwr, &qwr_init));

    ble_nus_init_t nus_init = {0};
    nus_init.data_handler = nus_evt;
    APP_ERROR_CHECK(ble_nus_init(&m_nus, &nus_init));

    memset(&m_vm_notify, 0, sizeof(m_vm_notify));
    m_vm_notify_wait_hvn_complete = false;
    m_run_after_hvn_complete = false;
    nrfclaw_vm_set_notify_handler(vm_notify_enqueue);

    /* Name each board as nRFClaw-XXXXXX, where XXXXXX are the last
     * three bytes of the BLE address as normally printed. */
    ble_gap_addr_t addr;
    APP_ERROR_CHECK(sd_ble_gap_addr_get(&addr));

    m_mac_suffix[0] = addr.addr[2];
    m_mac_suffix[1] = addr.addr[1];
    m_mac_suffix[2] = addr.addr[0];

    snprintf(m_device_name_ndp, sizeof(m_device_name_ndp),
             "nRFClaw(NDP)-%02X%02X%02X",
             m_mac_suffix[0], m_mac_suffix[1], m_mac_suffix[2]);

    snprintf(m_device_name_nus, sizeof(m_device_name_nus),
             "nRFClaw(NUS)-%02X%02X%02X",
             m_mac_suffix[0], m_mac_suffix[1], m_mac_suffix[2]);

    /* Application/NDP is the normal boot plane. */
    nrfclaw_ble_set_gap_name_ndp();

    ble_gap_conn_params_t conn_params = {0};
    conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    conn_params.slave_latency     = SLAVE_LATENCY;
    conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;
    APP_ERROR_CHECK(sd_ble_gap_ppcp_set(&conn_params));

    /*
     * Allocate/configure the single SoftDevice advertising set once.
     *
     * IMPORTANT:
     * We intentionally do NOT use the SDK ble_advertising state machine.
     * Its global BLE observer automatically reacts to disconnect events and
     * can restart FAST advertising even when the connection belongs to the
     * Application plane. Because Stage 8.1 shares one advertising set, that
     * behavior overwrites the Application advertising payload.
     *
     * The nRFClaw now owns this advertising handle directly.
     */
    APP_ERROR_CHECK(
        programming_adv_configure(false)
    );

    APP_ERROR_CHECK(
        app_timer_create(&m_discovery_timer,
                         APP_TIMER_MODE_SINGLE_SHOT,
                         discovery_timeout));

    APP_ERROR_CHECK(
        app_timer_create(&m_idle_timer,
                         APP_TIMER_MODE_SINGLE_SHOT,
                         idle_timeout));

    m_programming_allowed = false;
    m_session_connected_once = false;

    char msg[80];
    snprintf(msg, sizeof(msg),
             "BLE: ready as %s, advertising OFF\r\n",
             m_device_name_ndp);
    SEGGER_RTT_WriteString(0, msg);
}


/*
 * Reconfigure the single SoftDevice advertising set for NUS programming.
 *
 * nrfclaw_ble_app.c reuses the same adv_handle in application mode.
 * Therefore every time P0.21 opens programming we explicitly restore
 * the NUS advertising payload/parameters before starting advertising.
 */
static ret_code_t programming_adv_configure(bool start_now)
{
    nrfclaw_ble_set_gap_name_nus();
    ble_advdata_t adv = {0};
    ble_advdata_t scan_rsp = {0};

    ble_uuid_t nus_uuid = {
        BLE_UUID_NUS_SERVICE,
        m_nus.uuid_type
    };

    /*
     * R3.8.20c2e-mac1 programming advertising layout:
     *
     *   ADV           = flags + Nordic UART Service UUID
     *   SCAN RESPONSE = full board-specific name
     *
     * Put the connectable service UUID in the primary advertising packet.
     * CoreBluetooth can use scan-response data, but making NUS visible in the
     * primary ADV removes a platform-dependent discovery step before connect.
     * The full nRFClaw(NUS)-XXXXXX name still fits comfortably in SCAN_RSP.
     */
    adv.name_type = BLE_ADVDATA_NO_NAME;
    adv.include_appearance = false;
    adv.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    adv.uuids_complete.uuid_cnt = 1U;
    adv.uuids_complete.p_uuids = &nus_uuid;

    scan_rsp.name_type = BLE_ADVDATA_FULL_NAME;
    scan_rsp.include_appearance = false;


    uint16_t adv_len =
        sizeof(m_prog_adv_encoded);

    ret_code_t err =
        ble_advdata_encode(
            &adv,
            m_prog_adv_encoded,
            &adv_len
        );

    if (err != NRF_SUCCESS)
        return err;


    uint16_t scan_len =
        sizeof(m_prog_scan_encoded);

    err =
        ble_advdata_encode(
            &scan_rsp,
            m_prog_scan_encoded,
            &scan_len
        );

    if (err != NRF_SUCCESS)
        return err;


    ble_gap_adv_data_t data = {0};

    data.adv_data.p_data =
        m_prog_adv_encoded;

    data.adv_data.len =
        adv_len;

    data.scan_rsp_data.p_data =
        m_prog_scan_encoded;

    data.scan_rsp_data.len =
        scan_len;


    ble_gap_adv_params_t params = {0};

    params.properties.type =
        BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;

    params.primary_phy =
        BLE_GAP_PHY_1MBPS;

    params.duration =
        0U;

    params.interval =
        ADV_FAST_INTERVAL;

    params.filter_policy =
        BLE_GAP_ADV_FP_ANY;


    /*
     * On first boot m_adv_handle is NOT_SET and this allocates the one
     * advertising set supported by the Stage-8.1 design.
     *
     * Later P0.21 sessions simply reconfigure the same handle from
     * Application payload back to NUS payload.
     */
    err =
        sd_ble_gap_adv_set_configure(
            &m_adv_handle,
            &data,
            &params
        );

    if (err != NRF_SUCCESS)
        return err;

    err =
        sd_ble_gap_tx_power_set(
            BLE_GAP_TX_POWER_ROLE_ADV,
            m_adv_handle,
            NRFCLAW_BLE_DEFAULT_TX_POWER_DBM
        );

    if (err != NRF_SUCCESS)
        return err;


    if (!start_now)
        return NRF_SUCCESS;


    return sd_ble_gap_adv_start(
        m_adv_handle,
        APP_BLE_CONN_CFG_TAG
    );
}


static ret_code_t programming_adv_start(void)
{
    /*
     * Only P0.21 / an already authorized NUS session may call this.
     */
    return programming_adv_configure(true);
}


uint8_t nrfclaw_ble_shared_adv_handle(void)
{
    return m_adv_handle;
}


void nrfclaw_ble_programming_open(uint32_t seconds)
{
    m_programming_allowed = true;

    /*
     * If already connected, P0.21 acts only as an explicit session refresh.
     */
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
        m_session_connected_once = true;
        start_idle_timer();

        SEGGER_RTT_WriteString(
            0,
            "BLE: active session refreshed"
        );
        return;
    }

    /*
     * Fresh physical programming window:
     *
     * P0.21 -> FAST advertising -> 10 minutes to get the first connection.
     * The 5-minute command inactivity timer starts only after that first
     * connection succeeds.
     */
    m_session_connected_once = false;

    stop_timer_safely(m_discovery_timer);
    stop_timer_safely(m_idle_timer);

    ret_code_t err = app_timer_start(
        m_discovery_timer,
        APP_TIMER_TICKS(seconds * 1000UL),
        NULL
    );

    APP_ERROR_CHECK(err);

    err = programming_adv_start();

    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE) {
        APP_ERROR_CHECK(err);
    }

    SEGGER_RTT_WriteString(
        0,
        "BLE: FAST discovery OPEN for 60 s"
    );
}
/*
void nrfclaw_ble_programming_close(void)
{
    m_programming_allowed = false;
    m_session_connected_once = false;

    stop_timer_safely(m_discovery_timer);
    stop_timer_safely(m_idle_timer);

    if (m_conn_handle != BLE_CONN_HANDLE_INVALID) {
        ret_code_t err = sd_ble_gap_disconnect(
            m_conn_handle,
            BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);

        if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
            log_ret("disconnect", err);
    }

    advertising_idle();
}
*/

void nrfclaw_ble_programming_release(void)
{
    /*
     * B5.5 handoff: stop authorizing new NUS work but keep the current link
     * alive long enough for the command response.  When the CLI disconnects,
     * the existing DISCONNECTED path sees programming_allowed=false and
     * returns ownership to the Application/NDP plane.
     */
    m_programming_allowed = false;
    m_session_connected_once = false;
    stop_timer_safely(m_discovery_timer);
    stop_timer_safely(m_idle_timer);
}

void nrfclaw_ble_programming_close(void)
{
    ret_code_t err;

    /*
     * Sessão não está mais autorizada.
     */
    m_programming_allowed = false;
    m_session_connected_once = false;


    /*
     * Para todos os timers relacionados ao modo programação.
     */
    stop_timer_safely(
        m_discovery_timer
    );

    stop_timer_safely(
        m_idle_timer
    );


    /*
     * Cancela estados pendentes do NUS/VM.
     */
    m_vm_notify.active = false;
    m_vm_notify_wait_hvn_complete = false;
    m_run_after_hvn_complete = false;


    /*
     * Advertising OFF explicitamente.
     */
    err = sd_ble_gap_adv_stop(
        m_adv_handle
    );

    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE)
    {
        /*
         * Não usar RTT durante o teste de corrente.
         */
    }


    /*
     * Se houver central conectado, solicita disconnect.
     */
    if (m_conn_handle != BLE_CONN_HANDLE_INVALID)
    {
        err = sd_ble_gap_disconnect(
            m_conn_handle,
            BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION
        );

        if (err != NRF_SUCCESS &&
            err != NRF_ERROR_INVALID_STATE)
        {
            /*
             * Sem log.
             */
        }
    }
}

bool nrfclaw_ble_is_connected(void)
{
    return m_conn_handle != BLE_CONN_HANDLE_INVALID;
}

bool nrfclaw_ble_programming_active(void)
{
    return m_programming_allowed ||
           (m_conn_handle != BLE_CONN_HANDLE_INVALID);
}

void nrfclaw_ble_process(void)
{
    vm_notify_process();
}
