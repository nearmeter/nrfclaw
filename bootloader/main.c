#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "ble.h"
#include "ble_gap.h"
#include "ble_advdata.h"
#include "ble_srv_common.h"
#include "nrf_power.h"
#include "nrf_delay.h"
#include "nrf_nvic.h"
#include "nrf_mbr.h"
#include "nrf_sdm.h"
#include "app_util.h"
#include "nrfclaw_dfu_protocol.h"

#define APP_BLE_CONN_CFG_TAG 1
#define DEVICE_NAME_PREFIX "nRFClaw-DFU-"
#define PAGE_SIZE 4096U
#define RX_MAX (NRF_SDH_BLE_GATT_MAX_MTU_SIZE - 3U)
#define DFU_INFO_CAP_FLAG 0x80000000UL

#define BLE_UUID_NUS_SERVICE 0x0001U
#define BLE_UUID_NUS_RX_CHARACTERISTIC 0x0002U
#define BLE_UUID_NUS_TX_CHARACTERISTIC 0x0003U

static uint8_t m_nus_uuid_type;
static uint16_t m_nus_service_handle;
static ble_gatts_char_handles_t m_nus_rx_handles;
static ble_gatts_char_handles_t m_nus_tx_handles;
static uint16_t m_conn = BLE_CONN_HANDLE_INVALID;
static uint8_t m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
static uint8_t m_adv_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_scan_buf[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static volatile bool m_flash_done, m_flash_error;
static volatile bool m_pending;
static uint8_t m_pending_buf[RX_MAX];
static uint16_t m_pending_len;
static bool m_active;
static uint32_t m_size, m_expected_crc, m_rx_crc, m_next, m_erased_page = 0xFFFFFFFFUL;
static uint32_t m_first_page[PAGE_SIZE/4U];

typedef struct { uint32_t magic,size,crc,reserved; } meta_t;

static uint32_t crc32_step(uint32_t crc, uint8_t const *p, uint32_t n)
{
    while (n--) { crc ^= *p++; for (unsigned i=0;i<8;i++) crc=(crc>>1)^((crc&1U)?0xEDB88320UL:0U); }
    return crc;
}
static bool app_vector_valid(void)
{
    uint32_t sp=*(uint32_t const*)NRFCLAW_DFU_APP_START;
    uint32_t pc=*(uint32_t const*)(NRFCLAW_DFU_APP_START+4U);
    return ((sp&0x2FFE0000UL)==0x20000000UL) && pc>=NRFCLAW_DFU_APP_START && pc<NRFCLAW_DFU_APP_END && (pc&1U);
}
static bool app_meta_valid(void)
{
    meta_t const *m=(meta_t const*)NRFCLAW_DFU_META_ADDR;
    if(m->magic!=NRFCLAW_DFU_MAGIC || m->size==0U || m->size>NRFCLAW_DFU_APP_MAX_SIZE) return false;
    uint32_t crc=0xFFFFFFFFUL;
    crc=crc32_step(crc,(uint8_t const*)NRFCLAW_DFU_APP_START,m->size)^0xFFFFFFFFUL;
    return crc==m->crc;
}
static volatile uint32_t m_boot_error;
/* Fix4a diagnostics: values can be inspected with a debugger/J-Link. */
static volatile uint32_t m_ble_ram_start_required;
static volatile uint32_t m_ble_last_evt;
static volatile uint32_t m_ble_disconnect_reason;
static volatile uint32_t m_ble_att_mtu_requests;
static volatile uint32_t m_ble_sys_attr_missing;

static void boot_fail(uint8_t stage, uint32_t err)
{
    /* Debug value: 0xSSxxxxxx, where SS is the failed stage. */
    m_boot_error = ((uint32_t)stage << 24) | (err & 0x00FFFFFFUL);
    __disable_irq();
    while (1) { __WFE(); }
}

static void mbr_init_sd_or_fail(uint8_t stage)
{
    sd_mbr_command_t cmd = {0};
    cmd.command = SD_MBR_COMMAND_INIT_SD;
    uint32_t err = sd_mbr_command(&cmd);
    if (err != NRF_SUCCESS) boot_fail(stage, err);
}

__attribute__((naked, noreturn))
static void app_start_final(uint32_t sp, uint32_t pc)
{
    __asm volatile(
        "msr msp, r0\n"
        "movs r0, #0\n"
        "msr control, r0\n"
        "msr basepri, r0\n"
        "msr primask, r0\n"
        "isb\n"
        "bx r1\n"
    );
}

static void app_start(void)
{
    uint32_t sp = *(uint32_t const *)NRFCLAW_DFU_APP_START;
    uint32_t pc = *(uint32_t const *)(NRFCLAW_DFU_APP_START + 4U);
    uint8_t sd_enabled = 0;
    uint32_t err;

    /*
     * After reset the MBR forwards exceptions to the bootloader because
     * BOOTLOADERADDR is programmed. Restore the MBR -> SoftDevice SVC/IRQ
     * forwarding before using any sd_* API.
     */
    mbr_init_sd_or_fail(0xA1U);

    err = sd_softdevice_is_enabled(&sd_enabled);
    if (err != NRF_SUCCESS) boot_fail(0xA2U, err);
    if (sd_enabled) {
        err = sd_softdevice_disable();
        if (err != NRF_SUCCESS) boot_fail(0xA3U, err);
    }

    /* Tell the SoftDevice/MBR that forwarded application IRQs start here. */
    err = sd_softdevice_vector_table_base_set(NRFCLAW_DFU_APP_START);
    if (err != NRF_SUCCESS) boot_fail(0xA4U, err);

    __disable_irq();
    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    /*
     * Do not point VTOR directly at the application here. With a SoftDevice
     * present, SVC and interrupts must continue through the MBR/SoftDevice
     * forwarding path configured above. The application vector base has
     * already been registered with sd_softdevice_vector_table_base_set().
     *
     * app_start_final() also clears PRIMASK/BASEPRI before branching. This is
     * essential: entering the application with PRIMASK=1 makes the first
     * synchronous SoftDevice SVC (sd_softdevice_enable) escalate to HardFault.
     */
    __DSB();
    __ISB();
    app_start_final(sp, pc);
}
static void flash_evt(uint32_t evt, void *ctx)
{
    (void)ctx;
    if(evt==NRF_EVT_FLASH_OPERATION_SUCCESS)m_flash_done=true;
    else if(evt==NRF_EVT_FLASH_OPERATION_ERROR)m_flash_error=true;
}
NRF_SDH_SOC_OBSERVER(m_dfu_soc,0,flash_evt,NULL);
static bool flash_wait(void)
{
    while(!m_flash_done && !m_flash_error) (void)sd_app_evt_wait();
    bool ok=m_flash_done && !m_flash_error; m_flash_done=m_flash_error=false; return ok;
}
static bool erase_page(uint32_t addr)
{
    m_flash_done=m_flash_error=false;
    uint32_t e=sd_flash_page_erase(addr/PAGE_SIZE); if(e!=NRF_SUCCESS)return false; return flash_wait();
}
static bool write_words(uint32_t addr, uint8_t const *data, uint32_t len)
{
    /*
     * sd_flash_write() is intentionally fed from a small aligned RAM staging
     * buffer.  The original implementation rejected writes larger than
     * 64 words (256 bytes).  DFU END commits the buffered first application
     * page in one 4096-byte call, so the erase succeeded and the subsequent
     * write immediately returned false, leaving the vector page erased and
     * reporting NRFCLAW_DFU_FLASH_ERROR.
     *
     * Accept arbitrary lengths by splitting them into <=64-word operations.
     * Each operation is completed before the staging buffer is reused.
     */
    uint32_t tmp[64];
    uint32_t done=0U;

    while(done<len){
        uint32_t n=len-done;
        if(n>sizeof(tmp))n=sizeof(tmp);
        uint32_t words=(n+3U)/4U;

        for(uint32_t i=0;i<words;i++)tmp[i]=0xFFFFFFFFUL;
        memcpy(tmp,data+done,n);

        m_flash_done=m_flash_error=false;
        uint32_t e=sd_flash_write((uint32_t *)(addr+done),tmp,words);
        if(e!=NRF_SUCCESS)return false;
        if(!flash_wait())return false;

        done+=n;
    }
    return true;
}
static void notify(uint8_t cmd,uint8_t st,uint32_t next)
{
    if(m_conn==BLE_CONN_HANDLE_INVALID)return;
    uint8_t r[6]={cmd,st,(uint8_t)next,(uint8_t)(next>>8),(uint8_t)(next>>16),(uint8_t)(next>>24)};

    /*
     * DFU is stop-and-wait at the application layer: the host does not send
     * the next DATA block until this 6-byte ACK is received.  Do not silently
     * drop an ACK when the SoftDevice notification TX queue is temporarily
     * full.  That was the source of apparently random DATA timeouts after
     * tens of kilobytes: sd_ble_gatts_hvx() can return NRF_ERROR_RESOURCES,
     * and the old code discarded that return value.
     *
     * Wait for radio/SoftDevice progress and retry.  Existing flash_wait()
     * already relies on sd_app_evt_wait() while observers continue to run, so
     * this is safe in the same bootloader execution model.
     */
    for(;;){
        uint16_t n=sizeof(r);
        ble_gatts_hvx_params_t hvx={0};
        hvx.handle=m_nus_tx_handles.value_handle;
        hvx.type=BLE_GATT_HVX_NOTIFICATION;
        hvx.offset=0U;
        hvx.p_len=&n;
        hvx.p_data=r;

        uint32_t err=sd_ble_gatts_hvx(m_conn,&hvx);
        if(err==NRF_SUCCESS)return;
        if(err==NRF_ERROR_RESOURCES){
            (void)sd_app_evt_wait();
            if(m_conn==BLE_CONN_HANDLE_INVALID)return;
            continue;
        }
        if(err==NRF_ERROR_INVALID_STATE || err==BLE_ERROR_GATTS_SYS_ATTR_MISSING)return;
        m_boot_error=(0xBFUL<<24)|(err&0x00FFFFFFUL);
        return;
    }
}
static void process_frame(void)
{
    uint8_t b[RX_MAX]; uint16_t n;
    __disable_irq(); if(!m_pending){__enable_irq();return;} n=m_pending_len; memcpy(b,m_pending_buf,n); m_pending=false; __enable_irq();
    if(n<1U)return; uint8_t cmd=b[0];
    if(cmd==NRFCLAW_DFU_CMD_INFO){
        /*
         * Backward-compatible capability advertisement.  Older bootloaders
         * returned 0 while idle.  Fix4b sets bit31 and reports the maximum
         * firmware DATA payload (GATT value bytes minus cmd+offset = 5).
         * During an active transfer INFO still returns the current offset.
         */
        uint32_t info=m_active?m_next:(DFU_INFO_CAP_FLAG | (uint32_t)(RX_MAX-5U));
        notify(cmd,NRFCLAW_DFU_OK,info); return;
    }
    if(cmd==NRFCLAW_DFU_CMD_ABORT){m_active=false;notify(cmd,NRFCLAW_DFU_OK,0);return;}
    if(cmd==NRFCLAW_DFU_CMD_BOOT){notify(cmd,app_vector_valid()?NRFCLAW_DFU_OK:NRFCLAW_DFU_BAD_IMAGE,0); if(app_vector_valid()){nrf_delay_ms(100);sd_nvic_SystemReset();} return;}
    if(cmd==NRFCLAW_DFU_CMD_BEGIN){
        if(n!=9U){notify(cmd,NRFCLAW_DFU_BAD_LENGTH,0);return;}
        memcpy(&m_size,&b[1],4); memcpy(&m_expected_crc,&b[5],4);
        if(m_size==0U||m_size>NRFCLAW_DFU_APP_MAX_SIZE){notify(cmd,NRFCLAW_DFU_TOO_LARGE,0);return;}
        memset(m_first_page,0xFF,sizeof(m_first_page));m_rx_crc=0xFFFFFFFFUL;m_next=0;m_erased_page=0xFFFFFFFFUL;m_active=true;
        notify(cmd,NRFCLAW_DFU_OK,0);return;
    }
    if(cmd==NRFCLAW_DFU_CMD_DATA){
        if(!m_active){notify(cmd,NRFCLAW_DFU_BAD_STATE,m_next);return;} if(n<6U){notify(cmd,NRFCLAW_DFU_BAD_LENGTH,m_next);return;}
        uint32_t off;memcpy(&off,&b[1],4);uint32_t dlen=n-5U;
        if(off!=m_next || off+dlen>m_size){notify(cmd,NRFCLAW_DFU_BAD_OFFSET,m_next);return;}
        m_rx_crc=crc32_step(m_rx_crc,&b[5],dlen);
        if(off<PAGE_SIZE){ if(off+dlen>PAGE_SIZE){notify(cmd,NRFCLAW_DFU_BAD_OFFSET,m_next);m_active=false;return;} memcpy(((uint8_t*)m_first_page)+off,&b[5],dlen); }
        else {
            uint32_t addr=NRFCLAW_DFU_APP_START+off, page=addr&~(PAGE_SIZE-1U);
            if(page!=m_erased_page){if(!erase_page(page)){notify(cmd,NRFCLAW_DFU_FLASH_ERROR,m_next);m_active=false;return;}m_erased_page=page;}
            if(!write_words(addr,&b[5],dlen)){notify(cmd,NRFCLAW_DFU_FLASH_ERROR,m_next);m_active=false;return;}
        }
        m_next+=dlen;notify(cmd,NRFCLAW_DFU_OK,m_next);return;
    }
    if(cmd==NRFCLAW_DFU_CMD_END){
        if(!m_active||m_next!=m_size){notify(cmd,NRFCLAW_DFU_BAD_STATE,m_next);return;}
        uint32_t crc=m_rx_crc^0xFFFFFFFFUL;if(crc!=m_expected_crc){notify(cmd,NRFCLAW_DFU_BAD_CRC,m_next);m_active=false;return;}
        if(!erase_page(NRFCLAW_DFU_APP_START) || !write_words(NRFCLAW_DFU_APP_START,(uint8_t*)m_first_page,PAGE_SIZE)){notify(cmd,NRFCLAW_DFU_FLASH_ERROR,m_next);m_active=false;return;}
        meta_t meta={NRFCLAW_DFU_MAGIC,m_size,crc,0xFFFFFFFFUL};
        if(!erase_page(NRFCLAW_DFU_META_ADDR) || !write_words(NRFCLAW_DFU_META_ADDR,(uint8_t*)&meta,sizeof(meta))){notify(cmd,NRFCLAW_DFU_FLASH_ERROR,m_next);m_active=false;return;}
        m_active=false;notify(cmd,NRFCLAW_DFU_OK,m_next);nrf_delay_ms(150);sd_nvic_SystemReset();return;
    }
    notify(cmd,NRFCLAW_DFU_BAD_STATE,m_next);
}
static void nus_long_init(void)
{
    static ble_uuid128_t const base_uuid = {
        {0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,
         0x93,0xF3,0xA3,0xB5,0x00,0x00,0x40,0x6E}
    };
    uint32_t err=sd_ble_uuid_vs_add(&base_uuid,&m_nus_uuid_type);
    if(err!=NRF_SUCCESS)boot_fail(0xC1U,err);

    ble_uuid_t service_uuid={.uuid=BLE_UUID_NUS_SERVICE,.type=m_nus_uuid_type};
    err=sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY,&service_uuid,&m_nus_service_handle);
    if(err!=NRF_SUCCESS)boot_fail(0xC2U,err);

    ble_add_char_params_t cp={0};
    cp.uuid=BLE_UUID_NUS_RX_CHARACTERISTIC;
    cp.uuid_type=m_nus_uuid_type;
    cp.max_len=RX_MAX;
    cp.init_len=0U;
    cp.is_var_len=true;
    cp.char_props.write=1;
    cp.char_props.write_wo_resp=1;
    cp.write_access=SEC_OPEN;
    err=characteristic_add(m_nus_service_handle,&cp,&m_nus_rx_handles);
    if(err!=NRF_SUCCESS)boot_fail(0xC3U,err);

    memset(&cp,0,sizeof(cp));
    cp.uuid=BLE_UUID_NUS_TX_CHARACTERISTIC;
    cp.uuid_type=m_nus_uuid_type;
    cp.max_len=RX_MAX;
    cp.init_len=0U;
    cp.is_var_len=true;
    cp.char_props.notify=1;
    cp.cccd_write_access=SEC_OPEN;
    err=characteristic_add(m_nus_service_handle,&cp,&m_nus_tx_handles);
    if(err!=NRF_SUCCESS)boot_fail(0xC4U,err);
}

static void nus_long_on_write(ble_gatts_evt_write_t const *w)
{
    if(w->handle!=m_nus_rx_handles.value_handle)return;
    uint16_t n=w->len;
    if(n>RX_MAX || m_pending)return;
    memcpy(m_pending_buf,w->data,n);
    m_pending_len=n;
    m_pending=true;
}
static void ble_evt(ble_evt_t const *e, void *ctx)
{
    (void)ctx;
    m_ble_last_evt = e->header.evt_id;

    if(e->header.evt_id==BLE_GAP_EVT_CONNECTED){
        m_conn=e->evt.gap_evt.conn_handle;
    }
    else if(e->header.evt_id==BLE_GATTS_EVT_WRITE){
        nus_long_on_write(&e->evt.gatts_evt.params.write);
    }
    else if(e->header.evt_id==BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST){
        /*
         * The bootloader intentionally does not link nrf_ble_gatt.  In SDK17
         * that helper normally answers BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST.
         * BlueZ commonly asks for MTU 517 immediately after connecting.
         * If this event is ignored no ATT Exchange MTU Response is sent and
         * service discovery eventually times out/disconnects.
         *
         * DFU frames are deliberately sized for ATT MTU 23, so reply with the
         * configured server MTU instead of increasing the SoftDevice RAM use.
         */
        m_ble_att_mtu_requests++;
        uint32_t err=sd_ble_gatts_exchange_mtu_reply(
            e->evt.gatts_evt.conn_handle,
            NRF_SDH_BLE_GATT_MAX_MTU_SIZE);
        if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE){
            m_boot_error=(0xBDUL<<24)|(err&0x00FFFFFFUL);
        }
    }
    else if(e->header.evt_id==BLE_GATTS_EVT_SYS_ATTR_MISSING){
        /*
         * There is no Peer Manager/bond database in the tiny DFU bootloader.
         * Initialize empty system attributes so the NUS CCCD can be written
         * and notifications can be enabled by Bleak after discovery.
         */
        m_ble_sys_attr_missing++;
        uint32_t err=sd_ble_gatts_sys_attr_set(
            e->evt.gatts_evt.conn_handle,NULL,0,0);
        if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE){
            m_boot_error=(0xBEUL<<24)|(err&0x00FFFFFFUL);
        }
    }
    else if(e->header.evt_id==BLE_GAP_EVT_SEC_PARAMS_REQUEST){
        /*
         * The DFU transport is intentionally unbonded.  Reply explicitly
         * instead of leaving CoreBluetooth waiting for a security decision.
         */
        uint32_t err=sd_ble_gap_sec_params_reply(
            e->evt.gap_evt.conn_handle,
            BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
            NULL,
            NULL);
        if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE){
            m_boot_error=(0xBFUL<<24)|(err&0x00FFFFFFUL);
        }
    }
    else if(e->header.evt_id==BLE_GAP_EVT_PHY_UPDATE_REQUEST){
        /*
         * Keep DFU on the legacy 1M PHY.  This mirrors the MAC1 application
         * compatibility profile and avoids a PHY negotiation corner case in
         * CoreBluetooth while entering the independent bootloader BLE stack.
         */
        ble_gap_phys_t phys={
            .tx_phys=BLE_GAP_PHY_1MBPS,
            .rx_phys=BLE_GAP_PHY_1MBPS
        };
        uint32_t err=sd_ble_gap_phy_update(
            e->evt.gap_evt.conn_handle, &phys);
        if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE){
            m_boot_error=(0xC0UL<<24)|(err&0x00FFFFFFUL);
        }
    }
    else if(e->header.evt_id==BLE_GAP_EVT_DISCONNECTED){
        /*
         * A connect attempt stops connectable advertising.  If BlueZ/Bleak
         * aborts during service discovery or CCCD setup, keep the DFU
         * bootloader recoverable without requiring a hardware/system reset.
         */
        m_ble_disconnect_reason=e->evt.gap_evt.params.disconnected.reason;
        m_conn=BLE_CONN_HANDLE_INVALID;
        uint32_t err=sd_ble_gap_adv_start(m_adv_handle,APP_BLE_CONN_CFG_TAG);
        if(err!=NRF_SUCCESS && err!=NRF_ERROR_INVALID_STATE){
            m_boot_error=(0xBCUL<<24)|(err&0x00FFFFFFUL);
        }
    }
}
NRF_SDH_BLE_OBSERVER(m_ble_obs,3,ble_evt,NULL);
static void ble_init(void)
{
    uint32_t err;
    uint32_t ram = 0;

    /*
     * Critical bootloader bootstrap: after reset, MBR vectors point at the
     * bootloader. SD_MBR_COMMAND_INIT_SD restores forwarding to SoftDevice,
     * making SoftDevice SVCs (including sd_softdevice_enable) callable.
     */
    mbr_init_sd_or_fail(0xB1U);

    /* Once SD owns SVC/IRQs, forward application-class events to bootloader. */
    err = sd_softdevice_vector_table_base_set(NRFCLAW_DFU_BOOT_START);
    if (err != NRF_SUCCESS) boot_fail(0xB2U, err);

    err = nrf_sdh_enable_request();
    if (err != NRF_SUCCESS) boot_fail(0xB3U, err);
    err = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram);
    if (err != NRF_SUCCESS) boot_fail(0xB4U, err);

    /*
     * ram=0 above is intentional and follows the Nordic SDK17 examples:
     * nrf_sdh_ble_default_cfg_set() returns the SoftDevice-required RAM start.
     * Keep it for diagnostics and let nrf_sdh_ble_enable() validate it.
     * The bootloader linker uses the same 0x20004C28 RAM origin as the working
     * halfmoon application, so do not force the input to the linker address.
     */
    m_ble_ram_start_required=ram;
    err = nrf_sdh_ble_enable(&ram);
    m_ble_ram_start_required=ram;
    if (err != NRF_SUCCESS) boot_fail(0xB5U, err);

    static const char name[] = "nRFClaw-DFU";
    err = sd_ble_gap_device_name_set(NULL, (uint8_t const *)name, (uint16_t)(sizeof(name)-1U));
    if (err != NRF_SUCCESS) boot_fail(0xB6U, err);

    /* SDK17 ble_nus fixes its characteristic value length to the default
     * ATT payload (20 bytes), even when the SoftDevice is configured for a
     * larger MTU.  The DFU bootloader therefore owns a minimal NUS-compatible
     * service whose RX/TX value max_len follows RX_MAX (244 at MTU 247). */
    nus_long_init();

    /*
     * CoreBluetooth compatibility: advertise the NUS UUID in the primary
     * connectable packet so service-filtered discovery does not depend on a
     * scan response.  The complete DFU name easily fits in the scan response.
     * Keep legacy connectable/scannable advertising and the existing 100 ms
     * interval; no DFU transport or power-policy change is made here.
     */
    ble_advdata_t adv = {0};
    ble_advdata_t scan_rsp = {0};
    ble_uuid_t uu = {BLE_UUID_NUS_SERVICE, m_nus_uuid_type};

    adv.name_type = BLE_ADVDATA_NO_NAME;
    adv.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    adv.uuids_complete.uuid_cnt = 1;
    adv.uuids_complete.p_uuids = &uu;

    scan_rsp.name_type = BLE_ADVDATA_FULL_NAME;

    ble_gap_adv_data_t ad = {
        .adv_data = {.p_data=m_adv_buf, .len=sizeof(m_adv_buf)},
        .scan_rsp_data = {.p_data=m_scan_buf, .len=sizeof(m_scan_buf)}
    };
    err = ble_advdata_encode(&adv, ad.adv_data.p_data, &ad.adv_data.len);
    if (err != NRF_SUCCESS) boot_fail(0xB8U, err);
    err = ble_advdata_encode(&scan_rsp, ad.scan_rsp_data.p_data, &ad.scan_rsp_data.len);
    if (err != NRF_SUCCESS) boot_fail(0xBBU, err);

    ble_gap_adv_params_t ap = {0};
    ap.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    ap.interval = MSEC_TO_UNITS(100, UNIT_0_625_MS);
    ap.duration = 0;
    ap.filter_policy = BLE_GAP_ADV_FP_ANY;
    err = sd_ble_gap_adv_set_configure(&m_adv_handle, &ad, &ap);
    if (err != NRF_SUCCESS) boot_fail(0xB9U, err);
    err = sd_ble_gap_adv_start(m_adv_handle, APP_BLE_CONN_CFG_TAG);
    if (err != NRF_SUCCESS) boot_fail(0xBAU, err);
}
int main(void)
{
    uint32_t gp=NRF_POWER->GPREGRET; NRF_POWER->GPREGRET=0;
    if(gp!=NRFCLAW_DFU_GPREGRET_MAGIC && app_vector_valid()) app_start();
    ble_init(); while(1){process_frame();(void)sd_app_evt_wait();}
}
