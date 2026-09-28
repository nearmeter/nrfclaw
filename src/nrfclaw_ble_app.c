#include "nrfclaw_ble_app.h"
#include "nrfclaw_ble.h"
#include "nrfclaw_event.h"
#include "nrfclaw_ndp.h"
#include "nrfclaw_ndp_access.h"

#include "ble.h"
#include "ble_advdata.h"
#include "ble_srv_common.h"
#include "nrf_sdh_ble.h"
#include "nrf_error.h"
#include "app_error.h"
#include "app_timer.h"
#include <string.h>

/*
 * sdk_config.h used by the nRFClaw project exposes BLE observer
 * priorities 0..3. Programming/NUS already uses priority 3.
 *
 * Application BLE runs at priority 2 so both observers receive events,
 * while programming keeps the later priority used by the validated stack.
 */
#define APP_BLE_OBSERVER_PRIO 2
#define APP_CONN_CFG_TAG      1

#define APP_SERVICE_UUID      0xA800U
#define APP_RX_UUID           0xA801U
#define APP_TX_UUID           0xA802U

static const ble_uuid128_t m_base_uuid = {
    .uuid128 = { 0x47,0x9A,0x61,0x44,0x32,0x6A,0x4B,0xD2,
                 0xA7,0x10,0x00,0x00,0x43,0x4C,0x52,0x4E }
};

static nrfclaw_ble_app_role_t m_role;
static bool m_suspended;
static bool m_adv_active;
static bool m_notify_enabled;
static uint32_t m_tx_generation;
static uint16_t m_conn_handle;

/*
 * Pack 02 R2G reconnect fix.
 *
 * Do not restart connectable advertising directly from
 * BLE_GAP_EVT_DISCONNECTED.  On S132/BlueZ reconnect tests the link teardown
 * and the advertising-set transition can race.  Defer the restart to the
 * normal main-loop context and retry it until the shared Application plane is
 * advertising again.
 */
#define APP_ADV_RESTART_DELAY_MS  75U
#define APP_ADV_RETRY_DELAY_MS   150U
#define APP_ADV_ADAPTIVE_RETRY_MS 150U

APP_TIMER_DEF(m_adv_restart_timer);
APP_TIMER_DEF(m_adv_adaptive_timer);

static bool     m_adv_restart_pending;
static bool     m_adv_restart_due;
static uint32_t m_adv_restart_attempts;
static uint32_t m_adv_last_sd_error;

/* B7.6b: one-shot Direct-NDP disconnect policy. RAM only. */
static bool m_next_disconnect_low_power;
static bool m_adv_restart_low_power;

/*
 * R2G R3.1 three-state adaptive advertising.
 *
 * FAST   : short 100 ms discovery/reconnect burst.
 * NORMAL : medium 500 ms interval while recent activity is still likely.
 * SLOW   : low-power idle advertising.
 *
 * The timer callback never touches the SoftDevice.  It only marks a state
 * transition due; stop/reconfigure/start is always performed in main context.
 */
typedef enum
{
    APP_ADV_ADAPT_FAST = 0,
    APP_ADV_ADAPT_NORMAL,
    APP_ADV_ADAPT_SLOW
} app_adv_adaptive_state_t;

static bool     m_adv_adaptive_enabled;
static bool     m_adv_adaptive_due;
static bool     m_adv_adaptive_start_retry;
static uint16_t m_adv_fast_interval_ms;
static uint16_t m_adv_normal_interval_ms;
static uint16_t m_adv_slow_interval_ms;
static uint32_t m_adv_fast_window_ms;
static uint32_t m_adv_normal_window_ms;
static app_adv_adaptive_state_t m_adv_adaptive_state;

static void adv_adaptive_timer_handler(void *p_context)
{
    (void)p_context;
    m_adv_adaptive_due = true;
}

static void adv_adaptive_arm(uint32_t delay_ms)
{
    if (!m_adv_adaptive_enabled)
        return;

    m_adv_adaptive_due = false;

    ret_code_t err = app_timer_start(
        m_adv_adaptive_timer,
        APP_TIMER_TICKS(delay_ms),
        NULL
    );

    if (err != NRF_SUCCESS)
    {
        m_adv_adaptive_due = true;
        ((void)0);
    }
}

static void adv_restart_timer_handler(void *p_context)
{
    (void)p_context;
    /* app_timer callback only wakes the CPU and marks work for main context. */
    m_adv_restart_due = true;
}

static void adv_restart_schedule(uint32_t delay_ms)
{
    m_adv_restart_pending = true;
    m_adv_restart_due = false;

    ret_code_t err = app_timer_start(
        m_adv_restart_timer,
        APP_TIMER_TICKS(delay_ms),
        NULL
    );

    if (err != NRF_SUCCESS)
    {
        /* Never strand reconnect because the timer could not be armed. */
        m_adv_restart_due = true;
        ((void)0);
    }
}

static uint8_t m_uuid_type;
static uint16_t m_service_handle;
static ble_gatts_char_handles_t m_rx_handles;
static ble_gatts_char_handles_t m_tx_handles;

static uint8_t m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t m_adv_encoded[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_scan_encoded[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint16_t m_adv_interval_ms;
static int8_t m_tx_power_dbm;
static uint8_t m_user_payload[24];
static uint8_t m_user_payload_len;
/* B7.6d Direct Advertising Telemetry primary manufacturer payload. */
#define DIRECT_TELEMETRY_MAX_LEN 19U
static uint8_t m_direct_telemetry[DIRECT_TELEMETRY_MAX_LEN] = {
    0x4EU, 0x43U, 0x02U
};
static uint8_t m_direct_telemetry_len = DIRECT_TELEMETRY_MAX_LEN;

static uint8_t m_adv_name[24];
static uint8_t m_adv_name_len;

static nrfclaw_app_frame_t m_rx_mailbox;

static void push_event(nrfclaw_event_type_t type,uint32_t a0,uint32_t a1)
{
    nrfclaw_event_t e={.type=type,.arg0=a0,.arg1=a1};
    (void)nrfclaw_event_push(&e);
}

static uint32_t service_init(void)
{
    uint32_t err=sd_ble_uuid_vs_add(&m_base_uuid,&m_uuid_type);
    if(err!=NRF_SUCCESS)return err;

    ble_uuid_t uuid={.uuid=APP_SERVICE_UUID,.type=m_uuid_type};
    err=sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,&uuid,&m_service_handle);
    if(err!=NRF_SUCCESS)return err;

    /* RX: bridge -> nRFClaw, write / write without response. */
    ble_gatts_char_md_t cmd={0};
    cmd.char_props.write=1; cmd.char_props.write_wo_resp=1;
    ble_gatts_attr_md_t amd={0};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&amd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&amd.write_perm);
    amd.vloc = BLE_GATTS_VLOC_STACK;
    amd.vlen = 1U;
    ble_uuid_t cuuid={.uuid=APP_RX_UUID,.type=m_uuid_type};
    ble_gatts_attr_t attr={0};
    attr.p_uuid=&cuuid; attr.p_attr_md=&amd; attr.init_len=0;
    attr.init_offs=0; attr.max_len=sizeof(nrfclaw_app_frame_t); attr.p_value=NULL;
    err=sd_ble_gatts_characteristic_add(m_service_handle,&cmd,&attr,&m_rx_handles);
    if(err!=NRF_SUCCESS)return err;

    /* TX: nRFClaw -> bridge, notifications. */
    memset(&cmd,0,sizeof(cmd)); memset(&amd,0,sizeof(amd)); memset(&attr,0,sizeof(attr));
    cmd.char_props.notify=1;
    ble_gatts_attr_md_t cccd={0};
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd.write_perm);
    cccd.vloc=BLE_GATTS_VLOC_STACK;
    cmd.p_cccd_md=&cccd;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&amd.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&amd.write_perm);
    amd.vloc = BLE_GATTS_VLOC_STACK;
    amd.vlen = 1U;
    cuuid.uuid=APP_TX_UUID;
    attr.p_uuid=&cuuid; attr.p_attr_md=&amd; attr.init_len=0;
    attr.max_len=sizeof(nrfclaw_app_frame_t); attr.p_value=NULL;
    return sd_ble_gatts_characteristic_add(m_service_handle,&cmd,&attr,&m_tx_handles);
}

void nrfclaw_ble_app_init(void)
{
    m_next_disconnect_low_power = false;
    m_adv_restart_low_power = false;

    /*
     * S132/nRF52832: use the advertising set already allocated by the
     * programming/NUS layer. We intentionally never allocate a second set.
     */
    m_adv_handle = nrfclaw_ble_shared_adv_handle();

    m_role=NRFCLAW_BLE_APP_OFF; m_suspended=false; m_adv_active=false;
    m_notify_enabled=false; m_tx_generation=0U; m_conn_handle=BLE_CONN_HANDLE_INVALID;
    m_adv_restart_pending=false; m_adv_restart_due=false;
    m_adv_restart_attempts=0U; m_adv_last_sd_error=NRF_SUCCESS;
    m_adv_adaptive_enabled=false; m_adv_adaptive_due=false;
    m_adv_adaptive_start_retry=false;
    m_adv_fast_interval_ms=100U; m_adv_normal_interval_ms=500U;
    m_adv_slow_interval_ms=2000U;
    m_adv_fast_window_ms=3000U; m_adv_normal_window_ms=30000U;
    m_adv_adaptive_state=APP_ADV_ADAPT_FAST;
    m_adv_interval_ms=1000U; m_tx_power_dbm=4; m_user_payload_len=0; m_adv_name_len=0;
    memset(&m_rx_mailbox,0,sizeof(m_rx_mailbox));

    /*
     * Never ignore Application service initialization failures.
     *
     * Stage 8 adds a second Vendor Specific UUID base in addition to NUS.
     * sdk_config.h must therefore have:
     *
     *   NRF_SDH_BLE_VS_UUID_COUNT >= 2
     *
     * If the SoftDevice was configured with only one vendor UUID slot,
     * sd_ble_uuid_vs_add() fails and m_uuid_type remains invalid. The later
     * ble_advdata_encode() then reports NRF_ERROR_INVALID_PARAM (0x07).
     */
    uint32_t err = service_init();

    ((void)0);

    APP_ERROR_CHECK(err);

    err = app_timer_create(
        &m_adv_restart_timer,
        APP_TIMER_MODE_SINGLE_SHOT,
        adv_restart_timer_handler
    );
    APP_ERROR_CHECK(err);

    err = app_timer_create(
        &m_adv_adaptive_timer,
        APP_TIMER_MODE_SINGLE_SHOT,
        adv_adaptive_timer_handler
    );
    APP_ERROR_CHECK(err);
}

void nrfclaw_ble_app_suspend(void)
{
    m_next_disconnect_low_power = false;
    m_adv_restart_low_power = false;

    m_suspended=true;
    m_adv_restart_pending=false;
    m_adv_restart_due=false;
    (void)app_timer_stop(m_adv_restart_timer);
    m_adv_adaptive_due=false;
    m_adv_adaptive_start_retry=false;
    (void)app_timer_stop(m_adv_adaptive_timer);
    (void)nrfclaw_ble_app_adv_stop();
    if(m_conn_handle!=BLE_CONN_HANDLE_INVALID)
        (void)sd_ble_gap_disconnect(m_conn_handle,BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
}
void nrfclaw_ble_app_resume(void)
{
    m_suspended=false;
    if (m_adv_adaptive_enabled)
    {
        m_adv_adaptive_state = APP_ADV_ADAPT_FAST;
        m_adv_adaptive_start_retry = false;
        m_adv_interval_ms = m_adv_fast_interval_ms;
    }
}
bool nrfclaw_ble_app_is_suspended(void){return m_suspended;}

bool nrfclaw_ble_app_next_disconnect_low_power(void)
{
    if (m_suspended ||
        m_role != NRFCLAW_BLE_APP_PERIPHERAL ||
        m_conn_handle == BLE_CONN_HANDLE_INVALID)
        return false;

    m_next_disconnect_low_power = true;
    ((void)0);
    return true;
}

bool nrfclaw_ble_app_connected(void){return m_conn_handle!=BLE_CONN_HANDLE_INVALID;}

nrfclaw_ble_app_status_t nrfclaw_ble_app_set_role(nrfclaw_ble_app_role_t role)
{
    if(role>NRFCLAW_BLE_APP_SCANNER)return NRFCLAW_BLE_APP_BAD_ARG;
    if((role==NRFCLAW_BLE_APP_CENTRAL || role==NRFCLAW_BLE_APP_SCANNER)){
        m_role=role; return NRFCLAW_BLE_APP_UNSUPPORTED;
    }

    if(role==m_role)return NRFCLAW_BLE_APP_OK;

    /* r3.8.14b1: a VM BOOT program may take ownership of the Application
     * advertising set after the default NDP peripheral has already started.
     * Merely changing m_role is not enough: nrfclaw_ble_app_adv_start()
     * intentionally treats an already-active advertising set as success, so
     * the old NDP packet/interval would otherwise keep running.
     *
     * Stop the current set before changing role.  Also cancel adaptive NDP
     * advertising when leaving PERIPHERAL; otherwise its timer can later
     * rewrite the interval back to FAST/NORMAL/SLOW while a VM Beacon owns
     * the set. */
    if(m_conn_handle!=BLE_CONN_HANDLE_INVALID)return NRFCLAW_BLE_APP_BUSY;

    if(m_adv_active){
        nrfclaw_ble_app_status_t st=nrfclaw_ble_app_adv_stop();
        if(st!=NRFCLAW_BLE_APP_OK)return st;
    }

    if(role!=NRFCLAW_BLE_APP_PERIPHERAL){
        m_adv_adaptive_enabled=false;
        m_adv_adaptive_start_retry=false;
        (void)app_timer_stop(m_adv_adaptive_timer);
    }

    m_role=role;
    return NRFCLAW_BLE_APP_OK;
}
nrfclaw_ble_app_role_t nrfclaw_ble_app_role(void){return m_role;}


nrfclaw_ble_app_status_t nrfclaw_ble_app_adaptive_config(uint16_t fast_interval_ms,
                                                         uint32_t fast_window_ms,
                                                         uint16_t normal_interval_ms,
                                                         uint32_t normal_window_ms,
                                                         uint16_t slow_interval_ms)
{
    if (fast_interval_ms < 20U || fast_interval_ms > 10240U ||
        normal_interval_ms < 20U || normal_interval_ms > 10240U ||
        slow_interval_ms < 20U || slow_interval_ms > 10240U ||
        fast_window_ms < 100U || normal_window_ms < 100U)
        return NRFCLAW_BLE_APP_BAD_ARG;

    m_adv_fast_interval_ms = fast_interval_ms;
    m_adv_fast_window_ms = fast_window_ms;
    m_adv_normal_interval_ms = normal_interval_ms;
    m_adv_normal_window_ms = normal_window_ms;
    m_adv_slow_interval_ms = slow_interval_ms;
    m_adv_adaptive_enabled = true;
    m_adv_adaptive_start_retry = false;
    m_adv_adaptive_state = APP_ADV_ADAPT_FAST;
    m_adv_interval_ms = m_adv_fast_interval_ms;

    ((void)0);

    return NRFCLAW_BLE_APP_OK;
}

nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_config(uint16_t interval_ms,int8_t tx,const uint8_t *payload,uint8_t len)
{
    if(interval_ms<20U || interval_ms>10240U || len>sizeof(m_user_payload))return NRFCLAW_BLE_APP_BAD_ARG;
    m_adv_interval_ms=interval_ms; m_tx_power_dbm=tx; m_user_payload_len=len;
    if(len && payload)memcpy(m_user_payload,payload,len);
    return NRFCLAW_BLE_APP_OK;
}


nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_name(const uint8_t *name,uint8_t len)
{
    if(len==0U || len>sizeof(m_adv_name) || name==NULL)return NRFCLAW_BLE_APP_BAD_ARG;
    memcpy(m_adv_name,name,len); m_adv_name_len=len;
    return NRFCLAW_BLE_APP_OK;
}

uint16_t nrfclaw_ble_app_adv_interval_ms(void){return m_adv_interval_ms;}
int8_t nrfclaw_ble_app_adv_tx_power_dbm(void){return m_tx_power_dbm;}




bool nrfclaw_ble_app_telemetry_can_publish_now(void)
{
    return m_role == NRFCLAW_BLE_APP_PERIPHERAL &&
           !m_suspended &&
           m_conn_handle == BLE_CONN_HANDLE_INVALID &&
           m_adv_active &&
           (!m_adv_adaptive_enabled ||
            m_adv_adaptive_state == APP_ADV_ADAPT_SLOW);
}

bool nrfclaw_ble_app_telemetry_set(const uint8_t *payload, uint8_t len)
{
    nrfclaw_ble_app_status_t st;

    if (!payload || len == 0U || len > sizeof(m_direct_telemetry))
        return false;

    memcpy(m_direct_telemetry, payload, len);
    m_direct_telemetry_len = len;

    if (m_role != NRFCLAW_BLE_APP_PERIPHERAL ||
        m_suspended ||
        m_conn_handle != BLE_CONN_HANDLE_INVALID ||
        !m_adv_active)
        return true;

    /*
     * Do not lengthen FAST/NORMAL recovery. Cache the bytes and let the next
     * existing adaptive reconfigure consume them. In SLOW, one stop/start per
     * telemetry publication safely updates SoftDevice buffers in main context.
     */
    if (m_adv_adaptive_enabled &&
        m_adv_adaptive_state != APP_ADV_ADAPT_SLOW)
        return true;

    st = nrfclaw_ble_app_adv_stop();
    if (st != NRFCLAW_BLE_APP_OK)
        return false;

    st = nrfclaw_ble_app_adv_start();
    return st == NRFCLAW_BLE_APP_OK;
}

nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_start(void)
{
    ble_advdata_t adv={0};
    ble_advdata_t scan_rsp={0};
    ble_advdata_manuf_data_t manufacturer={0};
    uint16_t adv_len;
    uint16_t scan_len=0U;
    uint32_t err;

    if (m_suspended || m_role == NRFCLAW_BLE_APP_OFF)
        return NRFCLAW_BLE_APP_BUSY;
    if (m_adv_active)
        return NRFCLAW_BLE_APP_OK;


    if (m_role == NRFCLAW_BLE_APP_ADVERTISER && m_adv_name_len != 0U)
    {
        ble_gap_conn_sec_mode_t sec_mode;
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);
        if (sd_ble_gap_device_name_set(&sec_mode,m_adv_name,m_adv_name_len) != NRF_SUCCESS)
            return NRFCLAW_BLE_APP_BAD_ARG;
    }
    else
    {
        nrfclaw_ble_set_gap_name_ndp();
    }

    adv.flags=BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    if (m_role == NRFCLAW_BLE_APP_PERIPHERAL)
    {
        /* Primary packet is passive-runtime telemetry; scan response keeps
         * the full NDP name for CLI/user discovery. */
        manufacturer.company_identifier=0xFFFFU;
        manufacturer.data.p_data=m_direct_telemetry;
        manufacturer.data.size=m_direct_telemetry_len;
        adv.p_manuf_specific_data=&manufacturer;
        scan_rsp.name_type=BLE_ADVDATA_FULL_NAME;
    }
    else
    {

        if (m_adv_name_len != 0U)
            adv.name_type = BLE_ADVDATA_FULL_NAME;

        if (m_user_payload_len != 0U)
        {
            manufacturer.company_identifier=0xFFFFU;
            manufacturer.data.p_data=m_user_payload;
            manufacturer.data.size=m_user_payload_len;
            adv.p_manuf_specific_data=&manufacturer;
        }
    }

    adv_len=sizeof(m_adv_encoded);
    err=ble_advdata_encode(&adv,m_adv_encoded,&adv_len);
    if (err != NRF_SUCCESS)
        return NRFCLAW_BLE_APP_BAD_ARG;

    if (m_role == NRFCLAW_BLE_APP_PERIPHERAL)
    {
        scan_len=sizeof(m_scan_encoded);
        err=ble_advdata_encode(&scan_rsp,m_scan_encoded,&scan_len);
        if (err != NRF_SUCCESS)
            return NRFCLAW_BLE_APP_BAD_ARG;
    }

    ble_gap_adv_data_t data={0};
    data.adv_data.p_data=m_adv_encoded;
    data.adv_data.len=adv_len;
    if (scan_len != 0U)
    {
        data.scan_rsp_data.p_data=m_scan_encoded;
        data.scan_rsp_data.len=scan_len;
    }

    ble_gap_adv_params_t params={0};
    params.properties.type=(m_role == NRFCLAW_BLE_APP_PERIPHERAL)
        ? BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED
        : BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    params.primary_phy=BLE_GAP_PHY_1MBPS;
    params.duration=0U;
    params.interval=MSEC_TO_UNITS(m_adv_interval_ms,UNIT_0_625_MS);
    params.filter_policy=BLE_GAP_ADV_FP_ANY;

    if (m_adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET)
        return NRFCLAW_BLE_APP_RESOURCES;

    err=sd_ble_gap_adv_set_configure(&m_adv_handle,&data,&params);
    if (err != NRF_SUCCESS)
        return (err == NRF_ERROR_RESOURCES)
            ? NRFCLAW_BLE_APP_RESOURCES : NRFCLAW_BLE_APP_BUSY;

    (void)sd_ble_gap_tx_power_set(
        BLE_GAP_TX_POWER_ROLE_ADV,m_adv_handle,m_tx_power_dbm);

    err=sd_ble_gap_adv_start(m_adv_handle,APP_CONN_CFG_TAG);
    if (err != NRF_SUCCESS)
        return (err == NRF_ERROR_RESOURCES)
            ? NRFCLAW_BLE_APP_RESOURCES : NRFCLAW_BLE_APP_BUSY;

    m_adv_active=true;

    if (m_adv_adaptive_enabled)
    {
        if (m_adv_adaptive_state == APP_ADV_ADAPT_FAST &&
            m_adv_interval_ms == m_adv_fast_interval_ms)
        {
            adv_adaptive_arm(m_adv_fast_window_ms);
            ((void)0);
        }
        else if (m_adv_adaptive_state == APP_ADV_ADAPT_NORMAL &&
                 m_adv_interval_ms == m_adv_normal_interval_ms)
        {
            adv_adaptive_arm(m_adv_normal_window_ms);
            ((void)0);
        }
    }

    return NRFCLAW_BLE_APP_OK;
}

nrfclaw_ble_app_status_t nrfclaw_ble_app_adv_stop(void)
{
    if(!m_adv_active)return NRFCLAW_BLE_APP_OK;
    uint32_t err=sd_ble_gap_adv_stop(m_adv_handle);
    if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE)return NRFCLAW_BLE_APP_BUSY;
    m_adv_active=false; return NRFCLAW_BLE_APP_OK;
}

nrfclaw_ble_app_status_t nrfclaw_ble_app_connect(const uint8_t addr[6])
{(void)addr; return NRFCLAW_BLE_APP_UNSUPPORTED;}

nrfclaw_ble_app_status_t nrfclaw_ble_app_disconnect(void)
{
    if(m_conn_handle==BLE_CONN_HANDLE_INVALID)return NRFCLAW_BLE_APP_NOT_CONNECTED;
    uint32_t e=sd_ble_gap_disconnect(m_conn_handle,BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    return e==NRF_SUCCESS?NRFCLAW_BLE_APP_OK:NRFCLAW_BLE_APP_BUSY;
}

nrfclaw_ble_app_status_t nrfclaw_ble_app_send(const nrfclaw_app_frame_t *f)
{
    if(!f)return NRFCLAW_BLE_APP_BAD_ARG;
    if(m_suspended)return NRFCLAW_BLE_APP_BUSY;
    if(m_conn_handle==BLE_CONN_HANDLE_INVALID || !m_notify_enabled)return NRFCLAW_BLE_APP_NOT_CONNECTED;
    if(f->length>NRFCLAW_APP_MAX_PAYLOAD)return NRFCLAW_BLE_APP_BAD_ARG;
    uint8_t raw[6U+NRFCLAW_APP_MAX_PAYLOAD];
    raw[0]=NRFCLAW_APP_PROTO_VERSION;raw[1]=f->type;raw[2]=f->sequence;raw[3]=f->capability;raw[4]=f->operation;raw[5]=f->length;
    if(f->length)memcpy(&raw[6],f->payload,f->length);
    uint16_t len=(uint16_t)(6U+f->length);
    ble_gatts_hvx_params_t hvx={0}; hvx.handle=m_tx_handles.value_handle; hvx.type=BLE_GATT_HVX_NOTIFICATION; hvx.p_data=raw; hvx.p_len=&len;
    uint32_t e=sd_ble_gatts_hvx(m_conn_handle,&hvx);
    if(e==NRF_SUCCESS)return NRFCLAW_BLE_APP_OK;
    if(e==NRF_ERROR_RESOURCES)return NRFCLAW_BLE_APP_RESOURCES;
    return NRFCLAW_BLE_APP_BUSY;
}

uint32_t nrfclaw_ble_app_tx_generation(void){return m_tx_generation;}

nrfclaw_ble_app_status_t nrfclaw_ble_app_send_raw(const uint8_t *data,
                                                   uint16_t len)
{
    if (!data || len == 0U || len > 20U)
        return NRFCLAW_BLE_APP_BAD_ARG;
    if (m_suspended)
        return NRFCLAW_BLE_APP_BUSY;
    if (m_conn_handle == BLE_CONN_HANDLE_INVALID || !m_notify_enabled)
        return NRFCLAW_BLE_APP_NOT_CONNECTED;

    ble_gatts_hvx_params_t hvx = {0};
    hvx.handle = m_tx_handles.value_handle;
    hvx.type = BLE_GATT_HVX_NOTIFICATION;
    hvx.p_data = (uint8_t *)data;
    hvx.p_len = &len;

    uint32_t e = sd_ble_gatts_hvx(m_conn_handle, &hvx);
    if (e == NRF_SUCCESS)
        return NRFCLAW_BLE_APP_OK;
    if (e == NRF_ERROR_RESOURCES)
        return NRFCLAW_BLE_APP_RESOURCES;
    return NRFCLAW_BLE_APP_BUSY;
}

void nrfclaw_ble_app_on_rx(const uint8_t *data,uint16_t len)
{
    /*
     * Pack 02: NDP-SESSION is also accepted directly on the Application
     * GATT service. This gives Home Assistant a native, deterministic BLE
     * transport without opening the physical programming/NUS session.
     *
     * Program upload/auth remains on the validated NUS programming plane.
     */
    if (nrfclaw_ndp_is_session_frame(data, len))
    {
        uint8_t response[NRFCLAW_NDP_MAX_FRAME];
        uint16_t response_len = 0U;

        if (nrfclaw_ndp_handle_session_transport(data, len,
                                                 response, &response_len,
                                                 true))
        {
            (void)nrfclaw_ble_app_send_raw(response, response_len);
        }
        return;
    }

    if(!data || len<6U || data[0]!=NRFCLAW_APP_PROTO_VERSION)return;
    uint8_t n=data[5]; if(n>NRFCLAW_APP_MAX_PAYLOAD || (uint16_t)(6U+n)>len)return;
    m_rx_mailbox.version=data[0];m_rx_mailbox.type=data[1];m_rx_mailbox.sequence=data[2];m_rx_mailbox.capability=data[3];m_rx_mailbox.operation=data[4];m_rx_mailbox.length=n;
    if(n)memcpy(m_rx_mailbox.payload,&data[6],n);
    push_event(NRFCLAW_EVT_APP_COMMAND,((uint32_t)m_rx_mailbox.capability<<8)|m_rx_mailbox.operation,m_rx_mailbox.sequence);
}

bool nrfclaw_ble_app_last_frame(nrfclaw_app_frame_t *f){if(!f || m_rx_mailbox.version!=NRFCLAW_APP_PROTO_VERSION)return false;*f=m_rx_mailbox;return true;}


static nrfclaw_ble_app_status_t adv_resume(void)
{
    if (m_suspended)
        return NRFCLAW_BLE_APP_BUSY;

    if (m_role != NRFCLAW_BLE_APP_PERIPHERAL &&
        m_role != NRFCLAW_BLE_APP_ADVERTISER)
        return NRFCLAW_BLE_APP_BAD_ARG;

    if (m_adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET)
        return NRFCLAW_BLE_APP_RESOURCES;

    /*
     * Advertising data and parameters remain configured after a connection.
     * For reconnect, only restart the already configured advertising set.
     *
     * NRF_ERROR_INVALID_STATE means the set is already advertising; for the
     * single-owner Application plane that is equivalent to success.
     */
    uint32_t err = sd_ble_gap_adv_start(
        m_adv_handle,
        APP_CONN_CFG_TAG
    );

    m_adv_last_sd_error = err;

    if (err == NRF_SUCCESS ||
        err == NRF_ERROR_INVALID_STATE)
    {
        m_adv_active = true;
        return NRFCLAW_BLE_APP_OK;
    }

    if (err == NRF_ERROR_RESOURCES)
        return NRFCLAW_BLE_APP_RESOURCES;

    return NRFCLAW_BLE_APP_BUSY;
}

void nrfclaw_ble_app_process(void)
{
    if (m_adv_restart_pending && m_adv_restart_due)
    {
        /* Programming/NUS owns the shared advertising set while suspended. */
        if (m_suspended)
        {
            m_adv_restart_pending = false;
            m_adv_restart_due = false;
        }
        else if (m_role == NRFCLAW_BLE_APP_PERIPHERAL &&
                 m_conn_handle == BLE_CONN_HANDLE_INVALID)
        {
            m_adv_restart_due = false;
            m_adv_restart_attempts++;

            nrfclaw_ble_app_status_t status;

            if (m_adv_adaptive_enabled)
            {
                /*
                 * Reconfigure, do not merely resume, because the previous
                 * advertising set may already have fallen back to SLOW.
                 */
                m_adv_adaptive_state = m_adv_restart_low_power
                    ? APP_ADV_ADAPT_SLOW
                    : APP_ADV_ADAPT_FAST;
                m_adv_adaptive_start_retry = false;
                m_adv_interval_ms = m_adv_restart_low_power
                    ? m_adv_slow_interval_ms
                    : m_adv_fast_interval_ms;
                m_adv_active = false;
                status = nrfclaw_ble_app_adv_start();
            }
            else
            {
                status = adv_resume();
            }

            if (status == NRFCLAW_BLE_APP_OK)
            {
                ((void)0);

                m_adv_restart_pending = false;
                m_adv_restart_attempts = 0U;
                /* B7.6b consumed after successful restart. */
                m_adv_restart_low_power = false;
            }
            else
            {
                ((void)0);

                adv_restart_schedule(APP_ADV_RETRY_DELAY_MS);
            }
        }
    }

    if (!m_adv_adaptive_due)
        return;

    m_adv_adaptive_due = false;

    if (!m_adv_adaptive_enabled || m_suspended ||
        m_role != NRFCLAW_BLE_APP_PERIPHERAL ||
        m_conn_handle != BLE_CONN_HANDLE_INVALID)
        return;

    if (m_adv_adaptive_state == APP_ADV_ADAPT_SLOW &&
        m_adv_interval_ms == m_adv_slow_interval_ms && m_adv_active)
        return;

    /*
     * State transition: FAST -> NORMAL -> SLOW.
     * Reconfigure the shared advertising set only from main-loop context.
     * If stop/start temporarily fails, retry via a real app_timer wakeup.
     */
    app_adv_adaptive_state_t next_state = m_adv_adaptive_state;
    uint16_t next_interval_ms = m_adv_interval_ms;

    if (!m_adv_adaptive_start_retry)
    {
        if (m_adv_adaptive_state == APP_ADV_ADAPT_FAST)
        {
            next_state = APP_ADV_ADAPT_NORMAL;
            next_interval_ms = m_adv_normal_interval_ms;
        }
        else
        {
            next_state = APP_ADV_ADAPT_SLOW;
            next_interval_ms = m_adv_slow_interval_ms;
        }

        if (m_adv_active)
        {
            nrfclaw_ble_app_status_t stop_status = nrfclaw_ble_app_adv_stop();
            if (stop_status != NRFCLAW_BLE_APP_OK)
            {
                ((void)0);
                adv_adaptive_arm(APP_ADV_ADAPTIVE_RETRY_MS);
                return;
            }
        }

        m_adv_adaptive_state = next_state;
        m_adv_interval_ms = next_interval_ms;
    }

    nrfclaw_ble_app_status_t start_status = nrfclaw_ble_app_adv_start();
    if (start_status == NRFCLAW_BLE_APP_OK)
    {
        m_adv_adaptive_start_retry = false;
        if (m_adv_adaptive_state == APP_ADV_ADAPT_NORMAL)
        {
            ((void)0);
        }
        else
        {
            ((void)0);
        }
        return;
    }

    m_adv_adaptive_start_retry = true;
    ((void)0);
    adv_adaptive_arm(APP_ADV_ADAPTIVE_RETRY_MS);
}


static void ble_evt(ble_evt_t const *e,void *ctx)
{
    (void)ctx;
    switch(e->header.evt_id){
        case BLE_GAP_EVT_CONNECTED:
        {
            /*
             * Own every incoming link while the Application plane is the
             * active BLE peripheral.
             *
             * Do not depend on m_adv_active here. The SoftDevice stops
             * connectable advertising automatically when a connection is
             * established, and the flag can legitimately already be false.
             */
            if (!m_suspended &&
                m_role == NRFCLAW_BLE_APP_PERIPHERAL &&
                m_conn_handle == BLE_CONN_HANDLE_INVALID)
            {
                m_conn_handle =
                    e->evt.gap_evt.conn_handle;

                m_adv_active =
                    false;

                m_notify_enabled =
                    false;

                m_adv_restart_pending = false;
                m_adv_restart_attempts = 0U;
                m_adv_adaptive_due = false;
                m_adv_adaptive_start_retry = false;
                (void)app_timer_stop(m_adv_adaptive_timer);

                ((void)0);

                push_event(
                    NRFCLAW_EVT_APP_CONNECTED,
                    0,
                    0
                );
            }

            break;
        }


        case BLE_GAP_EVT_DISCONNECTED:
        {
            if (e->evt.gap_evt.conn_handle == m_conn_handle)
            {
                ((void)0);

                m_conn_handle =
                    BLE_CONN_HANDLE_INVALID;

                m_notify_enabled =
                    false;

                /* Authentication is connection-scoped. */
                nrfclaw_ndp_access_on_disconnect();

                m_adv_active =
                    false;

                push_event(
                    NRFCLAW_EVT_APP_DISCONNECTED,
                    0,
                    0
                );


                /*
                 * R2G reconnect fix:
                 * defer advertising restart until the SoftDevice has fully
                 * completed link teardown.  The main loop retries if the
                 * advertising set is temporarily unavailable.
                 *
                 * If P0.21 requested programming, suspend() is already true
                 * and NUS keeps ownership of the shared advertising handle.
                 */
                if (!m_suspended &&
                    m_role == NRFCLAW_BLE_APP_PERIPHERAL)
                {
                    m_adv_restart_low_power = m_next_disconnect_low_power;
                    m_next_disconnect_low_power = false;
                    if (m_adv_restart_low_power) {
                        ((void)0);
                    }

                    m_adv_restart_attempts = 0U;
                    adv_restart_schedule(APP_ADV_RESTART_DELAY_MS);

                    ((void)0);
                }
            }

            break;
        }
        case BLE_GATTS_EVT_WRITE:{
            ble_gatts_evt_write_t const *w=&e->evt.gatts_evt.params.write;
            if(e->evt.gatts_evt.conn_handle!=m_conn_handle)break;
            if(w->handle==m_rx_handles.value_handle)nrfclaw_ble_app_on_rx(w->data,w->len);
            else if(w->handle==m_tx_handles.cccd_handle && w->len==2U)m_notify_enabled=ble_srv_is_notification_enabled(w->data);
            break;}
        case BLE_GATTS_EVT_HVN_TX_COMPLETE:
            if(e->evt.gatts_evt.conn_handle==m_conn_handle){
                m_tx_generation++;
                if(m_tx_generation==0U)m_tx_generation=1U;
            }
            break;
        default:break;
    }
}
NRF_SDH_BLE_OBSERVER(m_app_ble_observer,APP_BLE_OBSERVER_PRIO,ble_evt,NULL);
