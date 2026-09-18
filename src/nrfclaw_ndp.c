#include "nrfclaw_ndp.h"
#include "nrfclaw_native.h"
#include "nrfclaw_battery.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_board_api.h"
#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ndp_key_store.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_ble_boot.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_lora_profile_store.h"
#include "nrfclaw_ninalink_lab.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_link.h"
#include <string.h>

#define NDP_INFO NRFCLAW_NDP_INFO
#define NDP_CAPS NRFCLAW_NDP_CAPS
#define NDP_STATUS NRFCLAW_NDP_STATUS
#define NDP_BOARD_INFO NRFCLAW_NDP_BOARD_INFO
#define NDP_BOARD_MEMORY NRFCLAW_NDP_BOARD_MEMORY
#define NDP_BOARD_RESET NRFCLAW_NDP_BOARD_RESET
#define NDP_BOARD_BOOTLOADER NRFCLAW_NDP_BOARD_BOOTLOADER
#define NDP_STATE_GET NRFCLAW_NDP_STATE_GET
#define NDP_SENSOR_READ NRFCLAW_NDP_SENSOR_READ
#define NDP_RADIO_GET NRFCLAW_NDP_RADIO_GET
#define NDP_RADIO_SET NRFCLAW_NDP_RADIO_SET
#define NDP_LORA_DIAG_SEND NRFCLAW_NDP_LORA_DIAG_SEND
#define NDP_LORA_DIAG_RX NRFCLAW_NDP_LORA_DIAG_RX
#define NDP_RADIO_GET_EXT NRFCLAW_NDP_RADIO_GET_EXT
#define NDP_RADIO_SET_EXT NRFCLAW_NDP_RADIO_SET_EXT
#define NDP_NINALINK_LAB NRFCLAW_NDP_NINALINK_LAB
#define NDP_NINALINK_BRIDGE NRFCLAW_NDP_NINALINK_BRIDGE
#define NDP_NINALINK_LINK NRFCLAW_NDP_NINALINK_LINK

typedef struct {
    bool active;
    uint8_t len;
    uint8_t offset;
    uint8_t data[48];
    int16_t rssi_x2;
    int16_t snr_x4;
} nrfclaw_lora_diag_chunk_t;

static nrfclaw_lora_diag_chunk_t m_lora_diag_chunk;

typedef struct {
    bool active;
    uint8_t len;
    uint8_t offset;
    uint8_t data[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    int16_t rssi_x2;
    int16_t snr_x4;
} nrfclaw_ninalink_bridge_chunk_t;

static nrfclaw_ninalink_bridge_chunk_t m_ninalink_bridge_chunk;


#define NDP_HALL_CONFIG NRFCLAW_NDP_HALL_CONFIG
#define NDP_ERROR NRFCLAW_NDP_ERROR
#define NDP_AUTH_BEGIN NRFCLAW_NDP_AUTH_BEGIN
#define NDP_AUTH_FINISH NRFCLAW_NDP_AUTH_FINISH
#define NDP_AUTH_LOGOUT NRFCLAW_NDP_AUTH_LOGOUT
#define NDP_ACCESS_STATUS NRFCLAW_NDP_ACCESS_STATUS
#define NDP_ACCEL_CONFIG NRFCLAW_NDP_ACCEL_CONFIG
#define NDP_VIB_INFO NRFCLAW_NDP_VIB_INFO
#define NDP_VIB_READ NRFCLAW_NDP_VIB_READ
#define NDP_ACCEL_PROBE NRFCLAW_NDP_ACCEL_PROBE
#define NDP_VIB_MODEL_SET NRFCLAW_NDP_VIB_MODEL_SET
#define NDP_VIB_HEALTH NRFCLAW_NDP_VIB_HEALTH
#define NDP_VIB_AUTO_START NRFCLAW_NDP_VIB_AUTO_START
#define NDP_VIB_AUTO_STOP NRFCLAW_NDP_VIB_AUTO_STOP
#define NDP_VIB_AUTO_STATUS NRFCLAW_NDP_VIB_AUTO_STATUS
#define NDP_VIB_AUTO_CONFIG NRFCLAW_NDP_VIB_AUTO_CONFIG
#define NDP_VIB_AUTO_BASELINE NRFCLAW_NDP_VIB_AUTO_BASELINE
#define NDP_VIB_AUTO_RESET NRFCLAW_NDP_VIB_AUTO_RESET
#define NDP_KEY_GENERATE NRFCLAW_NDP_KEY_GENERATE
#define NDP_KEY_GET NRFCLAW_NDP_KEY_GET
#define NDP_KEY_STATUS NRFCLAW_NDP_KEY_STATUS
#define NDP_BLE_BOOT_CONTROL NRFCLAW_NDP_BLE_BOOT_CONTROL
#define NDP_BLE_BEACON_CONTROL NRFCLAW_NDP_BLE_BEACON_CONTROL

#define NDP_OK NRFCLAW_NDP_OK
#define NDP_BAD_LENGTH NRFCLAW_NDP_BAD_LENGTH
#define NDP_BAD_ARG NRFCLAW_NDP_BAD_ARG
#define NDP_UNSUPPORTED NRFCLAW_NDP_UNSUPPORTED
#define NDP_BUSY NRFCLAW_NDP_BUSY
#define NDP_UNAUTHORIZED NRFCLAW_NDP_UNAUTHORIZED
#define NDP_FORBIDDEN NRFCLAW_NDP_FORBIDDEN
#define NDP_AUTH_FAILED NRFCLAW_NDP_AUTH_FAILED

#define NDP_SENSOR_BATTERY NRFCLAW_NDP_SENSOR_BATTERY
#define NDP_SENSOR_HALL NRFCLAW_NDP_SENSOR_HALL
#define NDP_SENSOR_ACCEL_XYZ NRFCLAW_NDP_SENSOR_ACCEL_XYZ
#define NDP_SENSOR_DS18B20 NRFCLAW_NDP_SENSOR_DS18B20
#define NDP_SENSOR_ACCEL_METRICS NRFCLAW_NDP_SENSOR_ACCEL_METRICS

bool nrfclaw_ndp_is_session_frame(uint8_t const*d,uint16_t len) {
    if(!d || len<4U) return false;
    /* Session flags currently reserve upper nibble; opcode >=32 avoids legacy CLI ambiguity. */
    return ((d[0]&0xF0U)==0U && d[1]>=32U && len==(uint16_t)(4U+d[3]));
}
static bool reply(uint8_t op,uint8_t seq,uint8_t st,uint8_t const*p,uint8_t n,uint8_t*out,uint16_t*ol) {
    if(!out||!ol||((uint16_t)n+5U)>NRFCLAW_NDP_MAX_FRAME) return false;
    out[0]=(st==NDP_OK)?0x01U:0x05U; /* RESPONSE; +ERROR */
    out[1]=op; out[2]=seq; out[3]=(uint8_t)(n+1U); out[4]=st;
    if(n&&p) memcpy(&out[5],p,n);
    *ol=(uint16_t)(5U+n); return true;
}
static bool access_allowed(bool enforce_auth, nrfclaw_ndp_access_level_t required)
{
    return !enforce_auth || nrfclaw_ndp_access_allowed(required);
}

static bool auth_exempt_opcode(uint8_t op)
{
    return op == NDP_AUTH_BEGIN || op == NDP_AUTH_FINISH ||
           op == NDP_AUTH_LOGOUT || op == NDP_ACCESS_STATUS;
}

static bool nus_owner_key_opcode(uint8_t op)
{
    return op == NDP_KEY_GENERATE || op == NDP_KEY_GET || op == NDP_KEY_STATUS ||
           op == NDP_BLE_BOOT_CONTROL || op == NDP_BLE_BEACON_CONTROL ||
           op == NDP_NINALINK_LAB || op == NDP_NINALINK_BRIDGE ||
           op == NDP_NINALINK_LINK;
}

bool nrfclaw_ndp_handle_session_transport(uint8_t const*d,uint16_t len,
                                           uint8_t*out,uint16_t*ol,
                                           bool enforce_auth) {
    if(!nrfclaw_ndp_is_session_frame(d,len)) return false;
    uint8_t op=d[1],seq=d[2],n=d[3]; uint8_t const*p=&d[4];
    uint8_t r[59]; uint8_t rn=0;

    /* R3.8 optional NDP protection:
     * - No stored key: Application/NDP remains open (pre-R3.7 behavior).
     * - Stored key: Application/NDP requires a CONTROL session.
     * - Physical P0.21/NUS always bypasses NDP authentication.
     * - Key generation/retrieval is physically gated and is NEVER exposed
     *   through the Application GATT plane, authenticated or not.
     */
    if (enforce_auth && nus_owner_key_opcode(op))
        return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
    if (enforce_auth && !auth_exempt_opcode(op) &&
        !nrfclaw_ndp_access_allowed(NRFCLAW_NDP_CONTROL))
        return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);

    switch(op) {
      case NDP_INFO: {
        nrfclaw_board_info_t bi;
        nrfclaw_board_get_info(&bi);
        r[rn++]=bi.ndp_version;
        r[rn++]=bi.vm_abi_version;
        r[rn++]=bi.board_type;
        return reply(op,seq,NDP_OK,r,rn,out,ol);
      }
      case NDP_CAPS: {
        uint16_t mask=0;
        for(uint8_t c=1;c<=13;c++) if(nrfclaw_native_capability_supported(c)) mask|=(uint16_t)(1U<<(c-1U));
        r[0]=(uint8_t)mask; r[1]=(uint8_t)(mask>>8);
        return reply(op,seq,NDP_OK,r,2,out,ol);
      }
      case NDP_STATUS: {
        uint8_t selector = (n == 0U) ? 0U : p[0];
        if (n > 1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if (selector == 0U) {
            /* Legacy/runtime summary -- wire compatible with R3.3/R3.4. */
            r[0]=(uint8_t)nrfclaw_hall_mode();
            r[1]=nrfclaw_native_ds18_active()?1U:0U;
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }
        if (selector == 1U) {
            /* Runtime ACTIVE bitmap. NDP_CAPS is the SUPPORTED bitmap. */
            uint16_t mask=0;
            for(uint8_t c=1;c<=13;c++) if(nrfclaw_native_capability_active(c)) mask|=(uint16_t)(1U<<(c-1U));
            r[0]=(uint8_t)mask; r[1]=(uint8_t)(mask>>8);
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }
        if(selector==2U){
            uint8_t h1,h2;uint32_t rc1,rc2;nrfclaw_hall_diag(&h1,&h2,&rc1,&rc2);
            r[0]=(uint8_t)nrfclaw_hall_mode();r[1]=nrfclaw_hall_channel();r[2]=h1;r[3]=h2;
            memcpy(&r[4],&rc1,4);memcpy(&r[8],&rc2,4);return reply(op,seq,NDP_OK,r,12,out,ol);
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_BOARD_INFO: {
        /*
         * Stage 10.2.3a-r1: keep each complete NDP-SESSION response <=20 bytes
         * so it fits the validated legacy NUS ATT payload without fragmentation.
         *
         * selector 0 (or empty): core identity, 15 bytes
         * selector 1: DEVICEID, 8 bytes
         * selector 2: git32, 4 bytes
         */
        uint8_t selector = (n == 0U) ? 0U : p[0];
        if (n > 1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        nrfclaw_board_info_t bi; nrfclaw_board_get_info(&bi);
        if (selector == 0U) {
            r[0]=bi.board_type; r[1]=bi.hw_rev_major; r[2]=bi.hw_rev_minor;
            r[3]=bi.fw_major; r[4]=bi.fw_minor; r[5]=bi.fw_patch;
            r[6]=bi.fw_prerelease;
            r[7]=(uint8_t)bi.fw_build; r[8]=(uint8_t)(bi.fw_build>>8);
            r[9]=bi.ndp_version; r[10]=bi.vm_abi_version;
            r[11]=bi.softdevice_family; r[12]=bi.softdevice_major;
            r[13]=bi.softdevice_minor; r[14]=bi.softdevice_patch;
            return reply(op,seq,NDP_OK,r,15,out,ol); /* total NUS = 20 */
        }
        if (selector == 1U) {
            r[0]=(uint8_t)bi.device_id0; r[1]=(uint8_t)(bi.device_id0>>8);
            r[2]=(uint8_t)(bi.device_id0>>16); r[3]=(uint8_t)(bi.device_id0>>24);
            r[4]=(uint8_t)bi.device_id1; r[5]=(uint8_t)(bi.device_id1>>8);
            r[6]=(uint8_t)(bi.device_id1>>16); r[7]=(uint8_t)(bi.device_id1>>24);
            return reply(op,seq,NDP_OK,r,8,out,ol);
        }
        if (selector == 2U) {
            r[0]=(uint8_t)bi.git32; r[1]=(uint8_t)(bi.git32>>8);
            r[2]=(uint8_t)(bi.git32>>16); r[3]=(uint8_t)(bi.git32>>24);
            return reply(op,seq,NDP_OK,r,4,out,ol);
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_BOARD_MEMORY: {
        /*
         * selector 0 (or empty): physical memory, 12 bytes
         * selector 1: nRFClaw flash layout, 14 bytes
         */
        uint8_t selector = (n == 0U) ? 0U : p[0];
        if (n > 1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        nrfclaw_board_memory_t bm; nrfclaw_board_get_memory(&bm);
        if (selector == 0U) {
            uint32_t f=bm.flash_total_bytes, ram=bm.ram_total_bytes;
            r[0]=(uint8_t)f; r[1]=(uint8_t)(f>>8);
            r[2]=(uint8_t)(f>>16); r[3]=(uint8_t)(f>>24);
            r[4]=(uint8_t)ram; r[5]=(uint8_t)(ram>>8);
            r[6]=(uint8_t)(ram>>16); r[7]=(uint8_t)(ram>>24);
            r[8]=(uint8_t)bm.flash_page_size; r[9]=(uint8_t)(bm.flash_page_size>>8);
            r[10]=(uint8_t)bm.flash_page_count; r[11]=(uint8_t)(bm.flash_page_count>>8);
            return reply(op,seq,NDP_OK,r,12,out,ol);
        }
        if (selector == 1U) {
            uint32_t a=bm.nrfclaw_data_start, b=bm.nrfclaw_data_end;
            uint32_t c=bm.nrfclaw_data_free_start;
            r[0]=(uint8_t)a; r[1]=(uint8_t)(a>>8); r[2]=(uint8_t)(a>>16); r[3]=(uint8_t)(a>>24);
            r[4]=(uint8_t)b; r[5]=(uint8_t)(b>>8); r[6]=(uint8_t)(b>>16); r[7]=(uint8_t)(b>>24);
            r[8]=(uint8_t)c; r[9]=(uint8_t)(c>>8); r[10]=(uint8_t)(c>>16); r[11]=(uint8_t)(c>>24);
            /* Current reserved free range fits u16; range-check future layouts. */
            if (bm.nrfclaw_data_free_bytes > 0xFFFFUL)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            r[12]=(uint8_t)bm.nrfclaw_data_free_bytes;
            r[13]=(uint8_t)(bm.nrfclaw_data_free_bytes>>8);
            return reply(op,seq,NDP_OK,r,14,out,ol); /* total NUS = 19 */
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_BOARD_RESET: {
        nrfclaw_board_reset_t br; nrfclaw_board_get_reset(&br);
        r[0]=(uint8_t)br.resetreas; r[1]=(uint8_t)(br.resetreas>>8);
        r[2]=(uint8_t)(br.resetreas>>16); r[3]=(uint8_t)(br.resetreas>>24);
        return reply(op,seq,NDP_OK,r,4,out,ol);
      }
      case NDP_BOARD_BOOTLOADER: {
        nrfclaw_board_bootloader_t bb; nrfclaw_board_get_bootloader(&bb);
        r[0]=bb.present?1U:0U; r[1]=(uint8_t)bb.address; r[2]=(uint8_t)(bb.address>>8);
        r[3]=(uint8_t)(bb.address>>16); r[4]=(uint8_t)(bb.address>>24);
        return reply(op,seq,NDP_OK,r,5,out,ol);
      }
      case NDP_KEY_STATUS:
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        r[0]=(uint8_t)nrfclaw_ndp_key_status();
        return reply(op,seq,NDP_OK,r,1,out,ol);
      case NDP_KEY_GENERATE:
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_ndp_key_generate()) return reply(op,seq,NDP_BUSY,0,0,out,ol);
        r[0]=(uint8_t)nrfclaw_ndp_key_status();
        return reply(op,seq,NDP_OK,r,1,out,ol);
      case NDP_KEY_GET: {
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_ndp_key_configured()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        uint8_t chunk=p[0];
        if(chunk>2U) return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        const uint8_t *key=nrfclaw_ndp_key();
        uint8_t off=(uint8_t)(chunk*15U);
        uint8_t count=(uint8_t)(NRFCLAW_NDP_KEY_SIZE-off);
        if(count>15U)count=15U;
        memcpy(r,&key[off],count);
        return reply(op,seq,NDP_OK,r,count,out,ol);
      }
      case NDP_ACCESS_STATUS:
        r[0]=(uint8_t)nrfclaw_ndp_access_level();
        return reply(op,seq,NDP_OK,r,1,out,ol);

      case NDP_AUTH_BEGIN:
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        {
            uint8_t sid=0U;
            if(!nrfclaw_ndp_access_begin(p[0],r,&sid))
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            r[12]=sid;
            return reply(op,seq,NDP_OK,r,13,out,ol); /* total 18 */
        }

      case NDP_AUTH_FINISH:
        /* tag16 only: NDP header(4) + tag(16) = 20-byte ATT payload. */
        if(n!=16U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_ndp_access_finish(p))
            return reply(op,seq,NDP_AUTH_FAILED,0,0,out,ol);
        r[0]=(uint8_t)nrfclaw_ndp_access_level();
        return reply(op,seq,NDP_OK,r,1,out,ol);

      case NDP_AUTH_LOGOUT:
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        nrfclaw_ndp_access_on_disconnect();
        return reply(op,seq,NDP_OK,0,0,out,ol);

      case NDP_SENSOR_READ:
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==NDP_SENSOR_BATTERY) {
            uint16_t cv;
            if(!nrfclaw_battery_last(&cv)) {
                if(!nrfclaw_battery_busy()) (void)nrfclaw_battery_start();
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            }
            r[0]=(uint8_t)cv; r[1]=(uint8_t)(cv>>8);
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }
        if(p[0]==NDP_SENSOR_HALL && nrfclaw_hall_active()) {
            uint32_t v=(nrfclaw_hall_mode()==NRFCLAW_HALL_MODE_QUADRATURE)?
                (uint32_t)nrfclaw_hall_position():nrfclaw_hall_count();
            r[0]=(uint8_t)v;r[1]=(uint8_t)(v>>8);r[2]=(uint8_t)(v>>16);r[3]=(uint8_t)(v>>24);
            return reply(op,seq,NDP_OK,r,4,out,ol);
        }
        if(p[0]==NDP_SENSOR_ACCEL_XYZ && nrfclaw_lis2dh12_present()) {
            nrfclaw_lis2dh12_xyz_t a; if(!nrfclaw_lis2dh12_read_xyz(&a)) return reply(op,seq,NDP_BUSY,0,0,out,ol);
            memcpy(&r[0],&a.x_mg,2);memcpy(&r[2],&a.y_mg,2);memcpy(&r[4],&a.z_mg,2);return reply(op,seq,NDP_OK,r,6,out,ol);
        }
        if(p[0]==NDP_SENSOR_DS18B20 && nrfclaw_ds18b20_present()) {
            int32_t mc; if(!nrfclaw_ds18b20_last_mC(&mc)){if(!nrfclaw_ds18b20_busy())(void)nrfclaw_ds18b20_start();return reply(op,seq,NDP_BUSY,0,0,out,ol);}
            memcpy(r,&mc,4);return reply(op,seq,NDP_OK,r,4,out,ol);
        }
        if(p[0]==NDP_SENSOR_ACCEL_METRICS) {
            nrfclaw_accel_vibration_metrics_t m;if(!nrfclaw_lis2dh12_vibration_metrics(&m))return reply(op,seq,NDP_BUSY,0,0,out,ol);
            memcpy(&r[0],&m.rms_mg,2);memcpy(&r[2],&m.peak_mg,2);memcpy(&r[4],&m.peak_to_peak_mg,2);memcpy(&r[6],&m.zero_cross_hz,2);return reply(op,seq,NDP_OK,r,8,out,ol);
        }
        return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
      case NDP_ACCEL_PROBE:
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        r[0]=nrfclaw_lis2dh12_present()?1U:0U;
        r[1]=nrfclaw_lis2dh12_i2c_address();
        r[2]=nrfclaw_lis2dh12_last_whoami();
        r[3]=(uint8_t)nrfclaw_lis2dh12_mode();
        return reply(op,seq,NDP_OK,r,4,out,ol);
      case NDP_ACCEL_CONFIG:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=9U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        { nrfclaw_accel_config_t c; c.mode=(nrfclaw_accel_mode_t)p[0];c.odr_hz=(uint16_t)p[1]|((uint16_t)p[2]<<8);c.full_scale_g=p[3];c.threshold_mg=(uint16_t)p[4]|((uint16_t)p[5]<<8);c.duration_ms=(uint16_t)p[6]|((uint16_t)p[7]<<8);c.low_power=(p[8]&1U)!=0;return reply(op,seq,nrfclaw_native_accel_configure(&c)==NRFCLAW_NATIVE_OK?NDP_OK:NDP_BAD_ARG,0,0,out,ol);}
      case NDP_VIB_INFO: {
        nrfclaw_accel_vibration_metrics_t m;if(!nrfclaw_lis2dh12_vibration_metrics(&m))return reply(op,seq,NDP_BUSY,0,0,out,ol);
        r[0]=m.sample_count;memcpy(&r[1],&m.sample_rate_hz,2);memcpy(&r[3],&m.rms_mg,2);memcpy(&r[5],&m.peak_mg,2);memcpy(&r[7],&m.peak_to_peak_mg,2);memcpy(&r[9],&m.zero_cross_hz,2);memcpy(&r[11],&m.sequence,4);return reply(op,seq,NDP_OK,r,15,out,ol);
      }
      case NDP_VIB_READ:
        if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        {uint8_t idx=p[0],cnt=p[1];if(cnt==0U||cnt>2U)return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);r[0]=idx;r[1]=0;uint8_t got=0;for(uint8_t i=0;i<cnt;i++){nrfclaw_lis2dh12_xyz_t a;if(!nrfclaw_lis2dh12_vibration_sample((uint8_t)(idx+i),&a))break;memcpy(&r[2+got*6],&a,6);got++;}r[1]=got;return reply(op,seq,got?NDP_OK:NDP_BAD_ARG,r,(uint8_t)(2+got*6),out,ol);}
      case NDP_VIB_MODEL_SET:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=16U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        {
            nrfclaw_vib_model_t model;
            model.rms_mean_mg=(uint16_t)p[0]|((uint16_t)p[1]<<8);
            model.rms_tol_mg=(uint16_t)p[2]|((uint16_t)p[3]<<8);
            model.peak_mean_mg=(uint16_t)p[4]|((uint16_t)p[5]<<8);
            model.peak_tol_mg=(uint16_t)p[6]|((uint16_t)p[7]<<8);
            model.p2p_mean_mg=(uint16_t)p[8]|((uint16_t)p[9]<<8);
            model.p2p_tol_mg=(uint16_t)p[10]|((uint16_t)p[11]<<8);
            model.zero_cross_mean_hz=(uint16_t)p[12]|((uint16_t)p[13]<<8);
            model.zero_cross_tol_hz=(uint16_t)p[14]|((uint16_t)p[15]<<8);
            return reply(op,seq,nrfclaw_vib_health_model_set(&model)?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
        }
      case NDP_VIB_HEALTH: {
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_vib_health_supported()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        nrfclaw_vib_model_t model;
        if(!nrfclaw_vib_health_model_get(&model)) return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        nrfclaw_vib_health_result_t h;
        if(!nrfclaw_vib_health_score(&h)) {
            /* First call starts a short FIFO window; subsequent polls remain
             * BUSY until INT2/vibration_capture() publishes metrics. */
            (void)nrfclaw_vib_health_prepare_sample();
            return reply(op,seq,NDP_BUSY,0,0,out,ol);
        }
        r[0]=h.model_valid?1U:0U; r[1]=(uint8_t)h.state;
        memcpy(&r[2],&h.score_q8_8,2);
        memcpy(&r[4],&h.metrics.rms_mg,2);
        memcpy(&r[6],&h.metrics.peak_mg,2);
        memcpy(&r[8],&h.metrics.peak_to_peak_mg,2);
        memcpy(&r[10],&h.metrics.zero_cross_hz,2);
        return reply(op,seq,NDP_OK,r,12,out,ol);
      }
      case NDP_VIB_AUTO_START:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_vib_auto_supported()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        return reply(op,seq,nrfclaw_vib_auto_start((p[0]&1U)!=0U)?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
      case NDP_VIB_AUTO_STOP:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_vib_auto_supported()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        nrfclaw_vib_auto_stop();return reply(op,seq,NDP_OK,0,0,out,ol);
      case NDP_VIB_AUTO_STATUS: {
        if(n>1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!nrfclaw_vib_auto_supported()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        nrfclaw_vib_auto_status_t st;nrfclaw_vib_auto_get_status(&st);
        uint8_t page=(n==0U)?0U:p[0];
        if(page==0U){
            r[0]=st.enabled?1U:0U;r[1]=st.discovery_complete?1U:0U;r[2]=(uint8_t)st.state;r[3]=st.profile_count;
            r[4]=st.candidate_count;r[5]=st.last_best_profile;r[6]=st.consecutive_bad;
            memcpy(&r[7],&st.last_score_q8_8,2);memcpy(&r[9],&st.last_metrics.rms_mg,2);memcpy(&r[11],&st.last_metrics.peak_mg,2);memcpy(&r[13],&st.next_sample_s,2);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }
        if(page==1U){
            memcpy(&r[0],&st.discovery_remaining_s,4);memcpy(&r[4],&st.last_metrics.peak_to_peak_mg,2);memcpy(&r[6],&st.last_metrics.zero_cross_hz,2);
            memcpy(&r[8],&st.stable_streak,2);memcpy(&r[10],&st.quiet_rms_mg,2);r[12]=st.persistence_busy?1U:0U;
            return reply(op,seq,NDP_OK,r,13,out,ol);
        }
        /* r3.8.2 discovery diagnostics. These pages are observational only and
         * intentionally do not alter the frozen page 0/1 status contract. */
        if(page==2U){
            memcpy(&r[0],&st.diag_total_windows,2);memcpy(&r[2],&st.diag_quiet_windows,2);memcpy(&r[4],&st.diag_active_windows,2);
            memcpy(&r[6],&st.diag_candidate_starts,2);memcpy(&r[8],&st.diag_candidate_matches,2);memcpy(&r[10],&st.diag_candidate_resets,2);memcpy(&r[12],&st.diag_profile_matches,2);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        if(page==3U){
            memcpy(&r[0],&st.diag_reject_rms,2);memcpy(&r[2],&st.diag_reject_peak,2);memcpy(&r[4],&st.diag_reject_p2p,2);memcpy(&r[6],&st.diag_reject_zc,2);
            memcpy(&r[8],&st.diag_last_candidate_score_q8_8,2);r[10]=st.diag_last_reject_feature;r[11]=st.diag_last_reject_mask;
            return reply(op,seq,NDP_OK,r,12,out,ol);
        }
        if(page==4U){
            r[0]=st.diag_candidate_valid?1U:0U;
            memcpy(&r[1],&st.diag_candidate_mean[0],2);memcpy(&r[3],&st.diag_candidate_mean[1],2);memcpy(&r[5],&st.diag_candidate_mean[2],2);memcpy(&r[7],&st.diag_candidate_mean[3],2);
            memcpy(&r[9],&st.diag_candidate_tol[0],2);memcpy(&r[11],&st.diag_candidate_tol[1],2);memcpy(&r[13],&st.diag_candidate_tol[2],2);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }
        if(page==5U){
            memcpy(&r[0],&st.diag_candidate_tol[3],2);
            memcpy(&r[2],&st.diag_last_feature_score_q8_8[0],2);memcpy(&r[4],&st.diag_last_feature_score_q8_8[1],2);memcpy(&r[6],&st.diag_last_feature_score_q8_8[2],2);memcpy(&r[8],&st.diag_last_feature_score_q8_8[3],2);
            return reply(op,seq,NDP_OK,r,10,out,ol);
        }
        /* r3.8.3 probe-pipeline/LIS2DH12 diagnostics. */
        if(page==6U){
            memcpy(&r[0],&st.diag_timer_fires,2);memcpy(&r[2],&st.diag_discovery_due,2);memcpy(&r[4],&st.diag_probe_requests,2);
            memcpy(&r[6],&st.diag_motion_releases,2);memcpy(&r[8],&st.diag_window_config_ok,2);memcpy(&r[10],&st.diag_window_config_fail,2);memcpy(&r[12],&st.diag_metrics_ready,2);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        if(page==7U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            r[0]=st.diag_probe_active?1U:0U;r[1]=st.diag_probe_stage;memcpy(&r[2],&st.diag_probe_age_s,2);
            r[4]=st.diag_sample_purpose;r[5]=st.diag_accel_mode;
            memcpy(&r[6],&ld.vibration_service_calls,2);memcpy(&r[8],&ld.fifo_empty_polls,2);memcpy(&r[10],&ld.capture_attempts,2);memcpy(&r[12],&ld.capture_success,2);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        if(page==8U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            memcpy(&r[0],&ld.reg_read_failures,2);memcpy(&r[2],&ld.reg_write_failures,2);memcpy(&r[4],&ld.sample_read_failures,2);
            memcpy(&r[6],&ld.int2_isr_count,2);memcpy(&r[8],&ld.int2_event_captures,2);r[10]=ld.last_fifo_src;r[11]=ld.last_fifo_count;
            return reply(op,seq,NDP_OK,r,12,out,ol);
        }
        /* r3.8.4: scheduler vs INT1 diagnostics. */
        if(page==9U){
            memcpy(&r[0],&st.diag_schedule_calls,2);memcpy(&r[2],&st.diag_timer_stop_calls,2);memcpy(&r[4],&st.diag_timer_start_ok,2);memcpy(&r[6],&st.diag_timer_start_fail,2);
            memcpy(&r[8],&st.diag_motion_events,2);memcpy(&r[10],&st.diag_motion_accepted,2);memcpy(&r[12],&st.diag_motion_ignored_state,2);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        if(page==10U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            memcpy(&r[0],&st.diag_arm_wake_calls,2);memcpy(&r[2],&st.diag_arm_wake_ok,2);memcpy(&r[4],&st.diag_arm_wake_fail,2);memcpy(&r[6],&st.diag_last_schedule_s,2);
            r[8]=st.diag_last_schedule_purpose;r[9]=st.diag_last_trigger;memcpy(&r[10],&ld.int1_isr_count,2);
            return reply(op,seq,NDP_OK,r,12,out,ol);
        }
        if(page==11U){
            memcpy(&r[0],&st.diag_arming_starts,2);memcpy(&r[2],&st.diag_arming_completes,2);memcpy(&r[4],&st.diag_motion_ignored_arming,2);memcpy(&r[6],&st.diag_int_first_windows,2);
            memcpy(&r[8],&st.diag_arming_remaining_s,2);r[10]=st.diag_arming_active?1U:0U;
            return reply(op,seq,NDP_OK,r,11,out,ol);
        }
        /* r3.8.6: low-power return state. Page 12 is the sensor
         * register snapshot captured immediately after HP-motion arm. Page 13
         * is a side-effect-free MCU/GPIO snapshot captured when queried. */
        if(page==12U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            memcpy(&r[0],&ld.motion_snapshot_count,2);memcpy(&r[2],&ld.motion_snapshot_fail,2);
            r[4]=ld.motion_ctrl1;r[5]=ld.motion_ctrl2;r[6]=ld.motion_ctrl3;r[7]=ld.motion_ctrl4;r[8]=ld.motion_ctrl5;r[9]=ld.motion_ctrl6;
            r[10]=ld.motion_fifo_ctrl;r[11]=ld.motion_int1_cfg;r[12]=ld.motion_int1_ths;r[13]=ld.motion_int1_duration;
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        if(page==13U){
            nrfclaw_lis2dh12_power_state_t ps;nrfclaw_lis2dh12_power_state_get(&ps);
            r[0]=ps.twi_enabled;r[1]=ps.int1_level;r[2]=ps.int2_level;r[3]=ps.gpiote_events_port;r[4]=ps.int1_latched;r[5]=ps.int2_latched;
            r[6]=ps.int1_sense;r[7]=ps.int2_sense;r[8]=ps.accel_mode;r[9]=ps.scl_level;r[10]=ps.sda_level;
            return reply(op,seq,NDP_OK,r,11,out,ol);
        }
        /* r3.8.9: TWI low-power cleanup audit. Page 14 contains only
         * side-effect-free MCU state plus the last post-transaction snapshot. */
        if(page==14U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            nrfclaw_lis2dh12_power_state_t ps;nrfclaw_lis2dh12_power_state_get(&ps);
            memcpy(&r[0],&ld.twi_low_power_releases,2);
            r[2]=ps.twi_psel_scl_disconnected;r[3]=ps.twi_psel_sda_disconnected;r[4]=ps.twi_errorsrc;r[5]=ps.hfclk_running;
            r[6]=ld.twi_last_psel_scl_disconnected;r[7]=ld.twi_last_psel_sda_disconnected;r[8]=ld.twi_last_errorsrc;r[9]=ld.twi_last_hfclk_running;
            r[10]=(uint8_t)(ps.scl_pin_cnf&0xFFU);r[11]=(uint8_t)(ps.sda_pin_cnf&0xFFU);
            r[12]=(uint8_t)(ld.twi_last_scl_pin_cnf&0xFFU);r[13]=(uint8_t)(ld.twi_last_sda_pin_cnf&0xFFU);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }
        /* r3.8.10: state-transition trace. Page 15 is summary; pages
         * 16..31 expose chronological trace entries 0..15. RAM-only. */
        if(page==15U){
            r[0]=nrfclaw_vib_auto_trace_count();r[1]=nrfclaw_vib_auto_last_arm_reason();
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }
        if(page>=16U && page<(16U+NRFCLAW_VIB_AUTO_TRACE_LEN)){
            nrfclaw_vib_auto_trace_entry_t te;
            if(!nrfclaw_vib_auto_trace_get((uint8_t)(page-16U),&te))return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            memcpy(&r[0],&te.timestamp_s,4);r[4]=te.event;r[5]=te.state;r[6]=te.purpose;r[7]=te.accel_mode;r[8]=te.arm_reason;
            return reply(op,seq,NDP_OK,r,9,out,ol);
        }
        /* r3.8.13: robust active-confirmation / impact rejection summary. */
        if(page==33U){
            nrfclaw_vib_auto_status_t vs;nrfclaw_vib_auto_get_status(&vs);
            memcpy(&r[0],&vs.diag_confirm_starts,2);memcpy(&r[2],&vs.diag_confirm_ok,2);
            memcpy(&r[4],&vs.diag_confirm_rejects,2);memcpy(&r[6],&vs.diag_confirm_delay_s,2);
            r[8]=vs.diag_confirm_pending?1U:0U;
            memcpy(&r[9],&vs.diag_confirm_retries,2);memcpy(&r[11],&vs.diag_confirm_retry_ok,2);
            memcpy(&r[13],&vs.diag_confirm_retry_delay_ms,2);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }
        /* r3.8.11: driver-level autonomous FIFO watchdog summary. */
        if(page==32U){
            nrfclaw_lis2dh12_diag_t ld;nrfclaw_lis2dh12_diag_get(&ld);
            memcpy(&r[0],&ld.fifo_watchdog_arms,2);memcpy(&r[2],&ld.fifo_watchdog_expirations,2);
            memcpy(&r[4],&ld.fifo_watchdog_retries,2);memcpy(&r[6],&ld.fifo_watchdog_aborts,2);
            memcpy(&r[8],&ld.fifo_int2_completions,2);memcpy(&r[10],&ld.fifo_timer_completions,2);
            memcpy(&r[12],&ld.fifo_last_active_ms,2);r[14]=ld.fifo_last_completion_source;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_BLE_BOOT_CONTROL: {
        /* Physical P0.21/NUS only. Payload: 0=status, 1=disable next boot,
         * 2=enable next boot. Setting is persistent. Current NUS session is
         * never disabled; a reset is intentionally required to apply. */
        if(n!=1U)return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==1U){if(!nrfclaw_ble_boot_set_ndp_enabled(false))return reply(op,seq,NDP_BUSY,0,0,out,ol);}
        else if(p[0]==2U){if(!nrfclaw_ble_boot_set_ndp_enabled(true))return reply(op,seq,NDP_BUSY,0,0,out,ol);}
        else if(p[0]!=0U)return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        r[0]=nrfclaw_ble_boot_ndp_enabled()?1U:0U;r[1]=nrfclaw_ble_boot_store_status();
        return reply(op,seq,NDP_OK,r,2,out,ol);
      }
      case NDP_BLE_BEACON_CONTROL: {
        /* Physical P0.21/NUS only. action 0=status, 1=configure/start after
         * NUS releases the shared advertising set, 2=stop/role OFF. */
        if(n<1U)return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==1U){
            if(n!=4U)return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            uint16_t interval=(uint16_t)p[1]|((uint16_t)p[2]<<8);int8_t tx=(int8_t)p[3];
            nrfclaw_ble_app_status_t bs=nrfclaw_ble_app_beacon_config(interval,tx);
            if(bs!=NRFCLAW_BLE_APP_OK)return reply(op,seq,bs==NRFCLAW_BLE_APP_BAD_ARG?NDP_BAD_ARG:NDP_BUSY,0,0,out,ol);
        } else if(p[0]==2U){
            if(n!=1U)return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            (void)nrfclaw_ble_app_adv_stop();(void)nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_OFF);
        } else if(p[0]!=0U || n!=1U)return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        r[0]=(uint8_t)nrfclaw_ble_app_role();
        uint16_t iv=nrfclaw_ble_app_adv_interval_ms();r[1]=(uint8_t)iv;r[2]=(uint8_t)(iv>>8);
        r[3]=(uint8_t)nrfclaw_ble_app_adv_tx_power_dbm();
        return reply(op,seq,NDP_OK,r,4,out,ol);
      }
      case NDP_VIB_AUTO_CONFIG: {
        if(!nrfclaw_vib_auto_supported()) return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        if(n==1U && (p[0]==0x80U || p[0]==0x81U || p[0]==0x82U)){
            nrfclaw_vib_auto_config_t c;nrfclaw_vib_auto_get_config(&c);
            if(p[0]==0x80U){
                memcpy(&r[0],&c.learning_time_s,4);memcpy(&r[4],&c.discovery_interval_s,2);memcpy(&r[6],&c.normal_interval_s,2);memcpy(&r[8],&c.suspicious_interval_s,2);
                memcpy(&r[10],&c.wake_threshold_mg,2);memcpy(&r[12],&c.wake_duration_ms,2);r[14]=c.confirm_delay_s;
                return reply(op,seq,NDP_OK,r,15,out,ol);
            }
            if(p[0]==0x82U){
                memcpy(&r[0],&c.arming_delay_s,4);
                return reply(op,seq,NDP_OK,r,4,out,ol);
            }
            memcpy(&r[0],&c.off_rms_mg,2);memcpy(&r[2],&c.profile_match_q8_8,2);memcpy(&r[4],&c.adapt_limit_q8_8,2);
            r[6]=c.candidate_confirmations;r[7]=c.alarm_consecutive;r[8]=c.max_profiles;r[9]=c.sensitivity;r[10]=c.adaptation_shift;r[11]=c.stable_expand_after;r[12]=c.max_interval_multiplier;
            return reply(op,seq,NDP_OK,r,13,out,ol);
        }
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        nrfclaw_vib_auto_config_t c;nrfclaw_vib_auto_get_config(&c);
        if(n==16U && p[0]==0U){
            memcpy(&c.learning_time_s,&p[1],4);c.discovery_interval_s=(uint16_t)p[5]|((uint16_t)p[6]<<8);c.normal_interval_s=(uint16_t)p[7]|((uint16_t)p[8]<<8);
            c.suspicious_interval_s=(uint16_t)p[9]|((uint16_t)p[10]<<8);c.wake_threshold_mg=(uint16_t)p[11]|((uint16_t)p[12]<<8);c.wake_duration_ms=(uint16_t)p[13]|((uint16_t)p[14]<<8);c.confirm_delay_s=p[15];c.arming_delay_s=c.confirm_delay_s;
            return reply(op,seq,nrfclaw_vib_auto_set_config(&c)?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
        }
        if(n==14U && p[0]==1U){
            c.off_rms_mg=(uint16_t)p[1]|((uint16_t)p[2]<<8);c.profile_match_q8_8=(uint16_t)p[3]|((uint16_t)p[4]<<8);c.adapt_limit_q8_8=(uint16_t)p[5]|((uint16_t)p[6]<<8);
            c.candidate_confirmations=p[7];c.alarm_consecutive=p[8];c.max_profiles=p[9];c.sensitivity=p[10];c.adaptation_shift=p[11];c.stable_expand_after=p[12];c.max_interval_multiplier=p[13];
            return reply(op,seq,nrfclaw_vib_auto_set_config(&c)?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
        }
        if(n==5U && p[0]==2U){
            memcpy(&c.arming_delay_s,&p[1],4);
            c.confirm_delay_s=(uint8_t)(c.arming_delay_s>255UL?255U:c.arming_delay_s);
            return reply(op,seq,nrfclaw_vib_auto_set_config(&c)?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
        }
        return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
      }
      case NDP_VIB_AUTO_BASELINE: {
        if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        nrfclaw_vib_auto_profile_t pr;if(!nrfclaw_vib_auto_get_profile(p[0],&pr))return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        if(p[1]==0U){
            r[0]=pr.confidence_pct;memcpy(&r[1],&pr.observations,2);memcpy(&r[3],&pr.rms_mean_mg,2);memcpy(&r[5],&pr.rms_tol_mg,2);
            memcpy(&r[7],&pr.peak_mean_mg,2);memcpy(&r[9],&pr.peak_tol_mg,2);return reply(op,seq,NDP_OK,r,11,out,ol);
        }
        if(p[1]==1U){
            memcpy(&r[0],&pr.p2p_mean_mg,2);memcpy(&r[2],&pr.p2p_tol_mg,2);memcpy(&r[4],&pr.zero_cross_mean_hz,2);memcpy(&r[6],&pr.zero_cross_tol_hz,2);return reply(op,seq,NDP_OK,r,8,out,ol);
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_VIB_AUTO_RESET:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL)) return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        return reply(op,seq,nrfclaw_vib_auto_reset_learning()?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
      case NDP_RADIO_GET: {
        nrfclaw_lora_profile_t lp; nrfclaw_lora_get_profile(&lp);
        memcpy(&r[0],&lp.frequency_hz,4); r[4]=(uint8_t)lp.power_dbm; r[5]=lp.sf;
        r[6]=(uint8_t)lp.bw_khz; r[7]=(uint8_t)(lp.bw_khz>>8); r[8]=lp.cr;
        return reply(op,seq,NDP_OK,r,9,out,ol);
      }
      case NDP_RADIO_SET:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=9U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        { nrfclaw_lora_profile_t lp;
          /* Legacy NDP v1 layout is frozen. Preserve extended fields. */
          nrfclaw_lora_get_profile(&lp);
          memcpy(&lp.frequency_hz,&p[0],4); lp.power_dbm=(int8_t)p[4]; lp.sf=p[5];
          lp.bw_khz=(uint16_t)p[6]|((uint16_t)p[7]<<8); lp.cr=p[8];
          return reply(op,seq,nrfclaw_lora_set_profile_persist(&lp)?NDP_OK:NDP_BAD_ARG,0,0,out,ol); }
      case NDP_RADIO_GET_EXT: {
        nrfclaw_lora_profile_t lp; nrfclaw_lora_get_profile(&lp);
        memcpy(&r[0],&lp.frequency_hz,4); r[4]=(uint8_t)lp.power_dbm; r[5]=lp.sf;
        r[6]=(uint8_t)lp.bw_khz; r[7]=(uint8_t)(lp.bw_khz>>8); r[8]=lp.cr;
        r[9]=lp.sync_word; r[10]=(uint8_t)lp.preamble_symbols; r[11]=(uint8_t)(lp.preamble_symbols>>8);
        r[12]=nrfclaw_lora_profile_store_busy()?1U:0U;
        { uint16_t g=(uint16_t)nrfclaw_lora_profile_store_generation(); r[13]=(uint8_t)g; r[14]=(uint8_t)(g>>8); }
        /* 15-byte body + status + 4-byte frame header = exactly 20 ATT bytes. */
        return reply(op,seq,NDP_OK,r,15,out,ol);
      }
      case NDP_RADIO_SET_EXT:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=12U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        { nrfclaw_lora_profile_t lp;
          memcpy(&lp.frequency_hz,&p[0],4); lp.power_dbm=(int8_t)p[4]; lp.sf=p[5];
          lp.bw_khz=(uint16_t)p[6]|((uint16_t)p[7]<<8); lp.cr=p[8];
          lp.sync_word=p[9]; lp.preamble_symbols=(uint16_t)p[10]|((uint16_t)p[11]<<8);
          return reply(op,seq,nrfclaw_lora_set_profile_persist(&lp)?NDP_OK:NDP_BAD_ARG,0,0,out,ol); }
      case NDP_LORA_DIAG_SEND:
        /* Physical NUS diagnostic: raw payload directly to LLCC68, no VM. */
        if(enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n==0U || n>48U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        return reply(op,seq,nrfclaw_lora_send_async(p,n)?NDP_OK:NDP_BUSY,0,0,out,ol);
      case NDP_LORA_DIAG_RX:
        /*
         * Physical NUS diagnostic.
         * action 0: start continuous RX
         * action 1: legacy whole-packet poll/take
         * action 2: cancel
         * action 3: B4.1 chunked poll/take for packets >9 bytes
         *
         * action 3 response body:
         * state:u8,total_len:u8,offset:u8,rssi_x2:i16,snr_x4:i16,data[0..8]
         * Max body = 15 bytes; plus 5-byte NDP response overhead = 20 bytes.
         */
        if(enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        if(p[0]==0U) {
            memset(&m_lora_diag_chunk,0,sizeof(m_lora_diag_chunk));
            if(nrfclaw_lora_rx_active()) return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,nrfclaw_lora_diag_stream_start()?NDP_OK:NDP_BUSY,0,0,out,ol);
        }

        if(p[0]==1U) {
            uint8_t dl=0U; int16_t rssi=0,snr=0;
            if(nrfclaw_lora_diag_stream_take(&r[6],&dl,48U,&rssi,&snr)) {
                r[0]=1U; r[1]=dl; memcpy(&r[2],&rssi,2); memcpy(&r[4],&snr,2);
                return reply(op,seq,NDP_OK,r,(uint8_t)(6U+dl),out,ol);
            }
            if(nrfclaw_lora_diag_stream_active()) { r[0]=0U; return reply(op,seq,NDP_OK,r,1,out,ol); }
            r[0]=2U; return reply(op,seq,NDP_OK,r,1,out,ol);
        }

        if(p[0]==2U) {
            memset(&m_lora_diag_chunk,0,sizeof(m_lora_diag_chunk));
            return reply(op,seq,nrfclaw_lora_cancel_receive()?NDP_OK:NDP_BAD_ARG,0,0,out,ol);
        }

        if(p[0]==3U) {
            if(!m_lora_diag_chunk.active) {
                uint8_t dl=0U; int16_t rssi=0,snr=0;
                if(nrfclaw_lora_diag_stream_take(
                       m_lora_diag_chunk.data,&dl,sizeof(m_lora_diag_chunk.data),
                       &rssi,&snr)) {
                    m_lora_diag_chunk.active=true;
                    m_lora_diag_chunk.len=dl;
                    m_lora_diag_chunk.offset=0U;
                    m_lora_diag_chunk.rssi_x2=rssi;
                    m_lora_diag_chunk.snr_x4=snr;
                } else {
                    if(nrfclaw_lora_diag_stream_active()) {
                        r[0]=0U;
                        return reply(op,seq,NDP_OK,r,1,out,ol);
                    }
                    r[0]=2U;
                    return reply(op,seq,NDP_OK,r,1,out,ol);
                }
            }

            {
                uint8_t remain=(uint8_t)(m_lora_diag_chunk.len-m_lora_diag_chunk.offset);
                uint8_t chunk=remain>8U?8U:remain;
                uint8_t off=m_lora_diag_chunk.offset;

                r[0]=1U;
                r[1]=m_lora_diag_chunk.len;
                r[2]=off;
                memcpy(&r[3],&m_lora_diag_chunk.rssi_x2,2);
                memcpy(&r[5],&m_lora_diag_chunk.snr_x4,2);
                memcpy(&r[7],&m_lora_diag_chunk.data[off],chunk);

                m_lora_diag_chunk.offset=(uint8_t)(off+chunk);
                if(m_lora_diag_chunk.offset>=m_lora_diag_chunk.len)
                    m_lora_diag_chunk.active=false;

                return reply(op,seq,NDP_OK,r,(uint8_t)(7U+chunk),out,ol);
            }
        }

        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      case NDP_NINALINK_BRIDGE: {
        nrfclaw_ninalink_bridge_status_t bs;
        nrfclaw_ninalink_app_dl_status_t as;

        if(enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n<1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        if(p[0]==0U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_status(&bs);
            r[0]=bs.active?1U:0U;
            r[1]=bs.queued;
            r[2]=(uint8_t)bs.received;r[3]=(uint8_t)(bs.received>>8);
            r[4]=(uint8_t)bs.valid;r[5]=(uint8_t)(bs.valid>>8);
            r[6]=(uint8_t)bs.invalid;r[7]=(uint8_t)(bs.invalid>>8);
            r[8]=(uint8_t)bs.dropped;r[9]=(uint8_t)(bs.dropped>>8);
            r[10]=(uint8_t)bs.radio_dropped;r[11]=(uint8_t)(bs.radio_dropped>>8);
            r[12]=bs.last_error;
            r[13]=(uint8_t)bs.ack_sent;r[14]=(uint8_t)(bs.ack_sent>>8);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==1U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memset(&m_ninalink_bridge_chunk,0,sizeof(m_ninalink_bridge_chunk));
            return reply(op,seq,
                         nrfclaw_ninalink_bridge_start()?NDP_OK:NDP_BUSY,
                         0,0,out,ol);
        }

        if(p[0]==2U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memset(&m_ninalink_bridge_chunk,0,sizeof(m_ninalink_bridge_chunk));
            nrfclaw_ninalink_bridge_stop();
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==3U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            if(!m_ninalink_bridge_chunk.active) {
                nrfclaw_ninalink_bridge_packet_t packet;
                if(nrfclaw_ninalink_bridge_take(&packet)) {
                    m_ninalink_bridge_chunk.active=true;
                    m_ninalink_bridge_chunk.len=packet.len;
                    m_ninalink_bridge_chunk.offset=0U;
                    memcpy(m_ninalink_bridge_chunk.data,packet.data,packet.len);
                    m_ninalink_bridge_chunk.rssi_x2=packet.rssi_x2;
                    m_ninalink_bridge_chunk.snr_x4=packet.snr_x4;
                } else {
                    nrfclaw_ninalink_bridge_get_status(&bs);
                    r[0]=bs.active?0U:2U;
                    return reply(op,seq,NDP_OK,r,1,out,ol);
                }
            }

            {
                uint8_t remain=(uint8_t)(
                    m_ninalink_bridge_chunk.len-m_ninalink_bridge_chunk.offset);
                uint8_t chunk=remain>8U?8U:remain;
                uint8_t off=m_ninalink_bridge_chunk.offset;

                r[0]=1U;
                r[1]=m_ninalink_bridge_chunk.len;
                r[2]=off;
                memcpy(&r[3],&m_ninalink_bridge_chunk.rssi_x2,2);
                memcpy(&r[5],&m_ninalink_bridge_chunk.snr_x4,2);
                memcpy(&r[7],&m_ninalink_bridge_chunk.data[off],chunk);

                m_ninalink_bridge_chunk.offset=(uint8_t)(off+chunk);
                if(m_ninalink_bridge_chunk.offset>=m_ninalink_bridge_chunk.len)
                    m_ninalink_bridge_chunk.active=false;

                return reply(op,seq,NDP_OK,r,(uint8_t)(7U+chunk),out,ol);
            }
        }

        if(p[0]==4U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_status(&bs);
            r[0]=(uint8_t)bs.duplicates;r[1]=(uint8_t)(bs.duplicates>>8);
            r[2]=(uint8_t)bs.ack_sent;r[3]=(uint8_t)(bs.ack_sent>>8);
            r[4]=(uint8_t)bs.ack_test_dropped;
            r[5]=(uint8_t)(bs.ack_test_dropped>>8);
            r[6]=bs.drop_next_ack?1U:0U;
            return reply(op,seq,NDP_OK,r,7,out,ol);
        }

        if(p[0]==5U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_drop_next_ack();
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==6U) {
            uint32_t node;
            bool active;
            if(n!=6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memcpy(&node,&p[1],4);
            active=p[5]?true:false;
            if(!nrfclaw_ninalink_bridge_queue_tracking(node,active))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==7U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_app_status(&as);
            r[0]=as.pending?1U:0U;
            memcpy(&r[1],&as.target_node,4);
            r[5]=(uint8_t)as.command_seq;r[6]=(uint8_t)(as.command_seq>>8);
            r[7]=as.requested_value?1U:0U;
            r[8]=as.state;
            r[9]=as.result;
            r[10]=(uint8_t)as.sent_count;r[11]=(uint8_t)(as.sent_count>>8);
            r[12]=(uint8_t)as.completed_count;r[13]=(uint8_t)(as.completed_count>>8);
            r[14]=as.timeout_count;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }

      case NDP_NINALINK_LINK: {
        nrfclaw_ninalink_link_status_t ls;
        nrfclaw_ninalink_app_status_t aps;

        if(enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n<1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        if(p[0]==0U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_link_get_status(&ls);
            r[0]=ls.state;
            r[1]=ls.result;
            r[2]=(uint8_t)ls.sequence;r[3]=(uint8_t)(ls.sequence>>8);
            r[4]=ls.tx_len;
            r[5]=ls.ack_len;
            memcpy(&r[6],&ls.ack_rssi_x2,2);
            memcpy(&r[8],&ls.ack_snr_x4,2);
            r[10]=(uint8_t)ls.acked_count;r[11]=(uint8_t)(ls.acked_count>>8);
            r[12]=(uint8_t)ls.timeout_count;r[13]=(uint8_t)(ls.timeout_count>>8);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }

        if(p[0]==1U) {
            if(n==3U) {
                uint16_t window_ms=(uint16_t)p[1]|((uint16_t)p[2]<<8);
                if(!nrfclaw_ninalink_link_start(window_ms))
                    return reply(op,seq,NDP_BUSY,0,0,out,ol);
            } else if(n==6U) {
                uint16_t window_ms=(uint16_t)p[1]|((uint16_t)p[2]<<8);
                uint8_t attempts=p[3];
                uint16_t backoff_ms=(uint16_t)p[4]|((uint16_t)p[5]<<8);
                if(!nrfclaw_ninalink_link_start_reliable(
                        window_ms,attempts,backoff_ms))
                    return reply(op,seq,NDP_BUSY,0,0,out,ol);
            } else {
                return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            }

            nrfclaw_ninalink_link_get_status(&ls);
            r[0]=ls.state;
            r[1]=ls.result;
            r[2]=(uint8_t)ls.sequence;r[3]=(uint8_t)(ls.sequence>>8);
            r[4]=ls.tx_len;
            r[5]=ls.ack_len;
            memcpy(&r[6],&ls.ack_rssi_x2,2);
            memcpy(&r[8],&ls.ack_snr_x4,2);
            r[10]=(uint8_t)ls.acked_count;r[11]=(uint8_t)(ls.acked_count>>8);
            r[12]=(uint8_t)ls.timeout_count;r[13]=(uint8_t)(ls.timeout_count>>8);
            return reply(op,seq,NDP_OK,r,14,out,ol);
        }

        if(p[0]==2U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_link_get_status(&ls);
            r[0]=ls.attempts;
            r[1]=ls.max_attempts;
            r[2]=(uint8_t)ls.retry_count;r[3]=(uint8_t)(ls.retry_count>>8);
            r[4]=(uint8_t)ls.timeout_count;r[5]=(uint8_t)(ls.timeout_count>>8);
            r[6]=(uint8_t)ls.base_backoff_ms;r[7]=(uint8_t)(ls.base_backoff_ms>>8);
            r[8]=(uint8_t)ls.last_backoff_ms;r[9]=(uint8_t)(ls.last_backoff_ms>>8);
            return reply(op,seq,NDP_OK,r,10,out,ol);
        }

        if(p[0]==3U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_link_get_app_status(&aps);
            r[0]=aps.valid?1U:0U;
            r[1]=(uint8_t)aps.sequence;r[2]=(uint8_t)(aps.sequence>>8);
            r[3]=aps.result;
            r[4]=(uint8_t)aps.applied_count;r[5]=(uint8_t)(aps.applied_count>>8);
            r[6]=(uint8_t)aps.duplicate_count;r[7]=(uint8_t)(aps.duplicate_count>>8);
            r[8]=aps.tracking_active?1U:0U;
            return reply(op,seq,NDP_OK,r,9,out,ol);
        }

        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }

      case NDP_NINALINK_LAB: {
        nrfclaw_ninalink_lab_status_t ls;
        if(enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n==0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==0U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        } else if(p[0]==1U) {
            if(n!=3U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            uint16_t period=(uint16_t)p[1]|((uint16_t)p[2]<<8);
            if(!nrfclaw_ninalink_lab_start(period))
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        } else if(p[0]==2U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_lab_stop();
        } else if(p[0]==3U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            if(!nrfclaw_ninalink_lab_send_max_test())
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
        } else {
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        }
        nrfclaw_ninalink_lab_get_status(&ls);
        r[0]=ls.active?1U:0U;
        r[1]=(uint8_t)ls.period_s;r[2]=(uint8_t)(ls.period_s>>8);
        r[3]=(uint8_t)ls.next_sequence;r[4]=(uint8_t)(ls.next_sequence>>8);
        r[5]=ls.last_entry_count;
        r[6]=ls.last_frame_length;
        r[7]=ls.last_result;
        r[8]=(uint8_t)ls.node_id;r[9]=(uint8_t)(ls.node_id>>8);
        r[10]=(uint8_t)(ls.node_id>>16);r[11]=(uint8_t)(ls.node_id>>24);
        return reply(op,seq,NDP_OK,r,12,out,ol);
      }
      case NDP_HALL_CONFIG:
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=4U && n!=5U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==0U) { nrfclaw_hall_disable(); return reply(op,seq,NDP_OK,0,0,out,ol); }
        { uint8_t channel=(n==5U)?p[4]:1U;
          nrfclaw_hall_config_t hc={(nrfclaw_hall_mode_t)p[0],channel,p[1]!=0,p[2]!=0,p[3]!=0};
          return reply(op,seq,nrfclaw_native_hall_configure(&hc)==NRFCLAW_NATIVE_OK?NDP_OK:NDP_BAD_ARG,0,0,out,ol); }
      default:
        return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
    }
}


bool nrfclaw_ndp_handle_session(uint8_t const*d,uint16_t len,uint8_t*out,uint16_t*ol)
{
    /* Conservative default for future callers: NDP is authenticated. */
    return nrfclaw_ndp_handle_session_transport(d,len,out,ol,true);
}
