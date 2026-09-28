#include "nrfclaw_ndp.h"
#include "nrfclaw_rtc.h"
#include "nrfclaw_native.h"
#include "nrfclaw_battery.h"
#include "nrfclaw_lora.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_board_api.h"
#include "nrfclaw_ndp_access.h"
#include "nrfclaw_ndp_key_store.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_temperature.h"
#include "nrfclaw_vib_health.h"
#include "nrfclaw_vib_auto.h"
#include "nrfclaw_ble_boot.h"
#include "nrfclaw_ha_role.h"
#include "nrfclaw_direct_sensor_control.h"
#include "nrfclaw_direct_hall_control.h"
#include "nrfclaw_vm_semantic_state.h"
#include "nrfclaw_vm.h"
#include "nrfclaw_state.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_lora_profile_store.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_node_registry.h"
#include "nrfclaw_ninalink_auto_discovery.h"
#include "nrfclaw_ninalink_state_cache.h"
#include "nrfclaw_ninalink_external.h"
#include "nrfclaw_ninalink_cap_inventory.h"
#include "nrfclaw_ninalink_query.h"
#include "nrfclaw_ble.h"
#include "nrfclaw_ninalink_external_subscription.h"
#include "nrfclaw_ninalink_link.h"
#include "nrfclaw_ninalink_network.h"
#include "nrfclaw_ninalink_telemetry.h"
#include "nrfclaw_ninalink_capability_discovery.h"
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
#define NDP_RADIO_GET_EXT NRFCLAW_NDP_RADIO_GET_EXT
#define NDP_RADIO_SET_EXT NRFCLAW_NDP_RADIO_SET_EXT
#define NDP_NINALINK_BRIDGE NRFCLAW_NDP_NINALINK_BRIDGE
#define NDP_BLE_DISCONNECT_HINT NRFCLAW_NDP_BLE_DISCONNECT_HINT
#define NDP_HA_ROLE_CONTROL NRFCLAW_NDP_HA_ROLE_CONTROL
#define NDP_DIRECT_SENSOR_CONTROL NRFCLAW_NDP_DIRECT_SENSOR_CONTROL
#define NDP_DIRECT_EVENT_SENSITIVITY NRFCLAW_NDP_DIRECT_EVENT_SENSITIVITY
#define NDP_DIRECT_HALL_CONTROL NRFCLAW_NDP_DIRECT_HALL_CONTROL
#define NDP_DIRECT_SEMANTIC_STATE NRFCLAW_NDP_DIRECT_SEMANTIC_STATE
#define NDP_NINALINK_NETWORK NRFCLAW_NDP_NINALINK_NETWORK


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
#define NDP_SENSOR_TEMPERATURE NRFCLAW_NDP_SENSOR_TEMPERATURE

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
    return op == NDP_KEY_GENERATE ||
           op == NDP_KEY_GET ||
           op == NDP_KEY_STATUS ||
           op == NDP_BLE_BOOT_CONTROL ||
           op == NDP_HA_ROLE_CONTROL ||
           op == NDP_NINALINK_NETWORK;
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
        for(uint8_t c=1;c<=NRFCLAW_CAP_TEMPERATURE;c++) if(nrfclaw_native_capability_supported(c)) mask|=(uint16_t)(1U<<(c-1U));
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
            for(uint8_t c=1;c<=NRFCLAW_CAP_TEMPERATURE;c++) if(nrfclaw_native_capability_active(c)) mask|=(uint16_t)(1U<<(c-1U));
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
        /* B5.5 logout clears external subscription. */
        nrfclaw_ninalink_external_subscription_reset();
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
        if(p[0]==NDP_SENSOR_TEMPERATURE && nrfclaw_temperature_available()) {
            int32_t mc;
            nrfclaw_temperature_source_t source;
            if(!nrfclaw_temperature_last_mC(&mc)) {
                if(!nrfclaw_temperature_busy()) (void)nrfclaw_temperature_start();
                if(!nrfclaw_temperature_last_mC(&mc))
                    return reply(op,seq,NDP_BUSY,0,0,out,ol);
            }
            source=nrfclaw_temperature_source();
            r[0]=(uint8_t)source;
            memcpy(&r[1],&mc,4);
            return reply(op,seq,NDP_OK,r,5,out,ol);
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
      case NDP_DIRECT_SENSOR_CONTROL:
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_ACCEL))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        if(n==0U) {
            r[0]=1U; /* payload contract version */
            r[1]=nrfclaw_direct_sensor_control_runtime_mode();
            r[2]=(uint8_t)nrfclaw_direct_sensor_control_persisted_mode();
            r[3]=nrfclaw_direct_sensor_control_has_persisted()?1U:0U;
            r[4]=nrfclaw_direct_sensor_control_store_status();
            return reply(op,seq,NDP_OK,r,5,out,ol);
        }
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(p[0]>(uint8_t)NRFCLAW_DIRECT_SENSOR_MODE_FALL)
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        if(nrfclaw_direct_sensor_control_store_status()!=0U)
            return reply(op,seq,NDP_BUSY,0,0,out,ol);
        if(!nrfclaw_direct_sensor_control_set_persist(
                (nrfclaw_direct_event_mode_t)p[0])) {
            if(nrfclaw_direct_sensor_control_store_status()!=0U)
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        }
        return reply(op,seq,NDP_OK,0,0,out,ol);
      case NDP_DIRECT_EVENT_SENSITIVITY:
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_ACCEL))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        if(n==0U) {
            r[0]=1U; /* payload contract version */
            r[1]=(uint8_t)nrfclaw_direct_sensor_control_sensitivity();
            r[2]=nrfclaw_direct_sensor_control_has_persisted_sensitivity()?1U:0U;
            r[3]=nrfclaw_direct_sensor_control_store_status();
            return reply(op,seq,NDP_OK,r,4,out,ol);
        }
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(p[0]>(uint8_t)NRFCLAW_DIRECT_SENSOR_SENSITIVITY_HIGH)
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        if(nrfclaw_direct_sensor_control_store_status()!=0U)
            return reply(op,seq,NDP_BUSY,0,0,out,ol);
        if(!nrfclaw_direct_sensor_control_set_sensitivity_persist(
                (nrfclaw_direct_event_sensitivity_t)p[0])) {
            if(nrfclaw_direct_sensor_control_store_status()!=0U)
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        }
        return reply(op,seq,NDP_OK,0,0,out,ol);
      case NDP_DIRECT_SEMANTIC_STATE: {
        nrfclaw_vm_semantic_state_t st;
        /* Frozen k4 GET remains payload-less and byte-for-byte compatible. */
        if(n==0U) {
            if(!nrfclaw_vm_semantic_state_snapshot(&st))
                return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
            r[0]=st.kind;
            r[1]=(uint8_t)st.capability_id;r[2]=(uint8_t)(st.capability_id>>8);
            r[3]=st.channel;r[4]=st.value_type;r[5]=(uint8_t)st.scale10;r[6]=st.unit;
            memcpy(&r[7],&st.raw_value,4);
            r[11]=(uint8_t)st.generation;r[12]=(uint8_t)(st.generation>>8);
            return reply(op,seq,NDP_OK,r,13,out,ol);
        }
        if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]==0U) {
            r[0]=1U; /* B7.6f2l3 control contract */
            r[1]=nrfclaw_vm_semantic_accumulator_resettable()?1U:0U;
            r[2]=(uint8_t)nrfclaw_state_status();
            return reply(op,seq,NDP_OK,r,3,out,ol);
        }
        if(p[0]==0xFFU) {
            if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
                return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
            if(nrfclaw_state_status()!=NRFCLAW_STATE_IDLE)
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            if(!nrfclaw_vm_semantic_accumulator_resettable())
                return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
            return reply(op,seq,
                         nrfclaw_vm_semantic_accumulator_reset()?NDP_OK:NDP_BUSY,
                         0,0,out,ol);
        }
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_DIRECT_HALL_CONTROL:
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_HALL))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        if(n==0U) {
            r[0]=1U; /* payload contract version */
            r[1]=(uint8_t)nrfclaw_direct_hall_control_runtime_mode();
            r[2]=nrfclaw_direct_hall_control_runtime_channel();
            r[3]=(uint8_t)nrfclaw_direct_hall_control_persisted_mode();
            r[4]=nrfclaw_direct_hall_control_persisted_channel();
            r[5]=nrfclaw_direct_hall_control_has_persisted()?1U:0U;
            r[6]=nrfclaw_direct_hall_control_store_status();
            return reply(op,seq,NDP_OK,r,7,out,ol);
        }
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n==1U && p[0]==0xFFU) {
            if(!nrfclaw_direct_hall_control_reset_value())
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }
        if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]>(uint8_t)NRFCLAW_HALL_MODE_QUADRATURE)
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        if(p[0]==(uint8_t)NRFCLAW_HALL_MODE_SINGLE && p[1]!=1U && p[1]!=2U)
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        if(nrfclaw_direct_hall_control_store_status()!=0U)
            return reply(op,seq,NDP_BUSY,0,0,out,ol);
        if(!nrfclaw_direct_hall_control_set_persist(
                (nrfclaw_hall_mode_t)p[0],p[1])) {
            if(nrfclaw_direct_hall_control_store_status()!=0U)
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        }
        return reply(op,seq,NDP_OK,0,0,out,ol);
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
        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
      }
      case NDP_NINALINK_NETWORK: {
        nrfclaw_ninalink_bridge_status_t bs;
        uint16_t network_id;

        /* B7.6f2l1: physical P0.21/NUS only. The v1 wire format already
         * carries network_id at bytes 4..5; this command only selects the
         * persistent value used by both node and bridge roles.
         * Payload: empty=status, u16 LE=set. */
        if(n==2U) {
            network_id=(uint16_t)p[0]|((uint16_t)p[1]<<8);
            if(nrfclaw_ninalink_network_store_status()!=0U)
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            if(!nrfclaw_ninalink_network_set_persist(network_id)) {
                if(nrfclaw_ninalink_network_store_status()!=0U)
                    return reply(op,seq,NDP_BUSY,0,0,out,ol);
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            }
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

        nrfclaw_ninalink_bridge_get_status(&bs);
        network_id=nrfclaw_ninalink_network_id();
        r[0]=1U; /* contract version */
        r[1]=(uint8_t)network_id;r[2]=(uint8_t)(network_id>>8);
        r[3]=nrfclaw_ninalink_network_has_persisted()?1U:0U;
        r[4]=nrfclaw_ninalink_network_store_status();
        r[5]=(uint8_t)bs.foreign_network;r[6]=(uint8_t)(bs.foreign_network>>8);
        return reply(op,seq,NDP_OK,r,7,out,ol);
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
      case NDP_HA_ROLE_CONTROL: {
        /* B7.6f physical-NUS-only persistent role control.
         * action 0=status, 1=NONE, 2=DIRECT_BLE, 3=NINALINK_NODE, 4=BRIDGE.
         * Setting is persisted; current programming/NUS ownership is never
         * interrupted. The BOOT VM/runtime role will apply after reset. */
        nrfclaw_ha_role_t role;
        if(n!=1U)return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        if(p[0]>=1U && p[0]<=4U){
            role=(nrfclaw_ha_role_t)(p[0]-1U);
            if(!nrfclaw_ha_role_set_persist(role))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
        } else if(p[0]!=0U) {
            return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
        }
        r[0]=(uint8_t)nrfclaw_ha_role_get();
        r[1]=nrfclaw_ha_role_store_status();
        return reply(op,seq,NDP_OK,r,2,out,ol);
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
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_LORA))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        nrfclaw_lora_profile_t lp; nrfclaw_lora_get_profile(&lp);
        memcpy(&r[0],&lp.frequency_hz,4); r[4]=(uint8_t)lp.power_dbm; r[5]=lp.sf;
        r[6]=(uint8_t)lp.bw_khz; r[7]=(uint8_t)(lp.bw_khz>>8); r[8]=lp.cr;
        return reply(op,seq,NDP_OK,r,9,out,ol);
      }
      case NDP_RADIO_SET:
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_LORA))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
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
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_LORA))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
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
        if(!nrfclaw_native_capability_supported(NRFCLAW_CAP_LORA))
            return reply(op,seq,NDP_UNSUPPORTED,0,0,out,ol);
        if(!access_allowed(enforce_auth, NRFCLAW_NDP_CONTROL))
            return reply(op,seq,NDP_UNAUTHORIZED,0,0,out,ol);
        if(n!=12U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        { nrfclaw_lora_profile_t lp;
          memcpy(&lp.frequency_hz,&p[0],4); lp.power_dbm=(int8_t)p[4]; lp.sf=p[5];
          lp.bw_khz=(uint16_t)p[6]|((uint16_t)p[7]<<8); lp.cr=p[8];
          lp.sync_word=p[9]; lp.preamble_symbols=(uint16_t)p[10]|((uint16_t)p[11]<<8);
          return reply(op,seq,nrfclaw_lora_set_profile_persist(&lp)?NDP_OK:NDP_BAD_ARG,0,0,out,ol); }
      case NDP_BLE_DISCONNECT_HINT:
        /* B7.6b: Application GATT only. The hint is RAM-only and applies
         * exactly to the next disconnect of this connection. */
        if(!enforce_auth) return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
        if(n!=0U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
        return reply(op,seq,
                     nrfclaw_ble_app_next_disconnect_low_power()
                         ? NDP_OK : NDP_BUSY,
                     0,0,out,ol);

      case NDP_NINALINK_BRIDGE: {
        nrfclaw_ninalink_bridge_status_t bs;
        nrfclaw_ninalink_app_dl_status_t as;
        nrfclaw_ninalink_command_dl_status_t cs;
        nrfclaw_ninalink_command_discovery_status_t ds;
        nrfclaw_ninalink_capability_discovery_status_t cds;

        /* B5.5 Application plane exposes only read-only/external selectors.

         * Top-level NDP access control still requires CONTROL when provisioned.

         * Bridge radio/test/clear operations remain physical-NUS-only. */
        if(enforce_auth &&
           (n<1U ||
            !(p[0]==8U || p[0]==9U ||
              (p[0]>=30U && p[0]<=37U) ||
              (p[0]>=40U && p[0]<=42U) ||
              p[0]==52U || p[0]==54U || p[0]==55U)))
            return reply(op,seq,NDP_FORBIDDEN,0,0,out,ol);
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
            return reply(op,seq,
                         nrfclaw_ninalink_bridge_start()?NDP_OK:NDP_BUSY,
                         0,0,out,ol);
        }

        if(p[0]==2U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_stop();
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }


        if(p[0]==8U) {
            uint32_t node;
            uint16_t command_id;
            uint8_t arg_len;

            if(n<8U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

            memcpy(&node,&p[1],4);
            command_id=(uint16_t)p[5]|((uint16_t)p[6]<<8);
            arg_len=p[7];

            if(arg_len>7U || n!=(uint8_t)(8U+arg_len))
                return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

            if(!nrfclaw_ninalink_bridge_queue_command(
                    node,command_id,&p[8],arg_len))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);

            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==9U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_command_status(&cs);

            r[0]=cs.pending?1U:0U;
            memcpy(&r[1],&cs.target_node,4);
            r[5]=(uint8_t)cs.command_seq;
            r[6]=(uint8_t)(cs.command_seq>>8);
            r[7]=(uint8_t)cs.command_id;
            r[8]=(uint8_t)(cs.command_id>>8);
            r[9]=cs.state;
            r[10]=cs.result;
            r[11]=(uint8_t)cs.sent_count;
            r[12]=(uint8_t)cs.completed_count;
            r[13]=cs.timeout_count;
            r[14]=cs.result_len;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==10U) {
            uint8_t off;
            uint8_t chunk;
            uint8_t remain;

            if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_command_status(&cs);

            off=p[1];
            if(off>cs.result_len)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);

            remain=(uint8_t)(cs.result_len-off);
            chunk=remain>8U?8U:remain;

            r[0]=cs.result_len;
            r[1]=off;
            if(chunk)
                memcpy(&r[2],&cs.result_data[off],chunk);

            return reply(op,seq,NDP_OK,r,(uint8_t)(2U+chunk),out,ol);
        }

        if(p[0]==11U) {
            uint32_t node;
            uint8_t start_index;

            if(n!=6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memcpy(&node,&p[1],4);
            start_index=p[5];

            if(!nrfclaw_ninalink_bridge_queue_command_discovery(
                    node,start_index))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);

            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==12U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_command_discovery_status(&ds);

            r[0]=ds.pending?1U:0U;
            memcpy(&r[1],&ds.target_node,4);
            r[5]=(uint8_t)ds.request_seq;
            r[6]=(uint8_t)(ds.request_seq>>8);
            r[7]=ds.start_index;
            r[8]=ds.state;
            r[9]=ds.registry_version;
            r[10]=ds.total_count;
            r[11]=ds.count;
            r[12]=(uint8_t)ds.sent_count;
            r[13]=(uint8_t)ds.completed_count;
            r[14]=ds.timeout_count;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==13U) {
            uint8_t index;
            nrfclaw_ninalink_command_descriptor_t const *d;

            if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_command_discovery_status(&ds);
            index=p[1];
            if(index>=ds.count)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);

            d=&ds.descriptors[index];
            r[0]=(uint8_t)d->command_id;
            r[1]=(uint8_t)(d->command_id>>8);
            r[2]=d->min_args;
            r[3]=d->max_args;
            r[4]=d->max_result;
            r[5]=d->flags;
            return reply(op,seq,NDP_OK,r,6,out,ol);
        }

        if(p[0]==14U) {
            uint32_t node;
            uint8_t page_index;
            if(n!=6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memcpy(&node,&p[1],4);
            page_index=p[5];
            if(!nrfclaw_ninalink_bridge_queue_capability_discovery(
                    node,page_index))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            return reply(op,seq,NDP_OK,0,0,out,ol);
        }

        if(p[0]==15U) {
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_capability_discovery_status(&cds);
            r[0]=cds.pending?1U:0U;
            memcpy(&r[1],&cds.target_node,4);
            r[5]=(uint8_t)cds.request_seq;
            r[6]=(uint8_t)(cds.request_seq>>8);
            r[7]=cds.page_index;
            r[8]=cds.state;
            r[9]=cds.registry_version;
            r[10]=cds.count;
            r[11]=cds.more?1U:0U;
            r[12]=(uint8_t)cds.sent_count;
            r[13]=(uint8_t)cds.completed_count;
            r[14]=cds.timeout_count;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==16U) {
            uint8_t index;
            nrfclaw_ninalink_capability_descriptor_t const *d;
            if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_capability_discovery_status(&cds);
            index=p[1];
            if(index>=cds.count)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            d=&cds.descriptors[index];
            r[0]=(uint8_t)d->desc.capability_id;
            r[1]=(uint8_t)(d->desc.capability_id>>8);
            r[2]=d->desc.channel;
            r[3]=d->desc.kind;
            r[4]=d->desc.value_type;
            r[5]=(uint8_t)d->desc.scale10;
            r[6]=d->desc.unit;
            r[7]=d->desc.behavior_flags;
            r[8]=d->runtime_state_flags;
            return reply(op,seq,NDP_OK,r,9,out,ol);
        }

        if(p[0]==17U) {
            nrfclaw_ninalink_node_registry_status_t rs;
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_node_registry_get_status(&rs);
            r[0]=rs.count;
            r[1]=rs.capacity;
            r[2]=(uint8_t)rs.updates;r[3]=(uint8_t)(rs.updates>>8);
            r[4]=(uint8_t)rs.creations;r[5]=(uint8_t)(rs.creations>>8);
            r[6]=(uint8_t)rs.evictions;r[7]=(uint8_t)(rs.evictions>>8);
            return reply(op,seq,NDP_OK,r,8,out,ol);
        }


        /* B5.4 External State/Event Interface v1. */
        if(p[0]==30U) {
            nrfclaw_ninalink_external_status_t xs54;
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_external_get_status(&xs54);
            r[0]=xs54.schema_version;r[1]=xs54.node_count;
            r[2]=xs54.state_value_count;r[3]=xs54.event_count;
            r[4]=(uint8_t)xs54.oldest_event_id;
            r[5]=(uint8_t)(xs54.oldest_event_id>>8);
            r[6]=(uint8_t)(xs54.oldest_event_id>>16);
            r[7]=(uint8_t)(xs54.oldest_event_id>>24);
            r[8]=(uint8_t)xs54.newest_event_id;
            r[9]=(uint8_t)(xs54.newest_event_id>>8);
            r[10]=(uint8_t)(xs54.newest_event_id>>16);
            r[11]=(uint8_t)(xs54.newest_event_id>>24);
            return reply(op,seq,NDP_OK,r,12,out,ol);
        }

        if(p[0]==31U) {
            nrfclaw_ninalink_external_node_t xn54;
            if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            if(!nrfclaw_ninalink_external_get_node(p[1],nrfclaw_rtc_now(),&xn54))
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            r[0]=(uint8_t)xn54.node_id;r[1]=(uint8_t)(xn54.node_id>>8);
            r[2]=(uint8_t)(xn54.node_id>>16);r[3]=(uint8_t)(xn54.node_id>>24);
            r[4]=xn54.state_count;r[5]=xn54.event_count;r[6]=xn54.discovery_state;
            r[7]=(uint8_t)xn54.age_s;r[8]=(uint8_t)(xn54.age_s>>8);
            memcpy(&r[9],&xn54.rssi_x2,2);memcpy(&r[11],&xn54.snr_x4,2);
            r[13]=xn54.flags;r[14]=xn54.last_message_type;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==32U) {
            nrfclaw_ninalink_external_state_t xv54;
            uint32_t node54;
            if(n!=6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            node54=((uint32_t)p[1])|((uint32_t)p[2]<<8)|
                   ((uint32_t)p[3]<<16)|((uint32_t)p[4]<<24);
            if(!nrfclaw_ninalink_external_get_state(node54,p[5],nrfclaw_rtc_now(),&xv54))
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            r[0]=(uint8_t)xv54.capability_id;r[1]=(uint8_t)(xv54.capability_id>>8);
            r[2]=xv54.channel;r[3]=xv54.value_type;
            r[4]=(uint8_t)xv54.raw_value;r[5]=(uint8_t)(xv54.raw_value>>8);
            r[6]=(uint8_t)(xv54.raw_value>>16);r[7]=(uint8_t)(xv54.raw_value>>24);
            r[8]=(uint8_t)xv54.sequence;r[9]=(uint8_t)(xv54.sequence>>8);
            r[10]=(uint8_t)xv54.age_s;r[11]=(uint8_t)(xv54.age_s>>8);
            r[12]=(uint8_t)xv54.update_count;r[13]=(uint8_t)(xv54.update_count>>8);
            r[14]=xv54.source_message_type;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==33U) {
            nrfclaw_ninalink_external_event_meta_t xe54;
            uint32_t cursor54;bool found54;
            if(n!=5U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            cursor54=((uint32_t)p[1])|((uint32_t)p[2]<<8)|
                     ((uint32_t)p[3]<<16)|((uint32_t)p[4]<<24);
            memset(&xe54,0,sizeof(xe54));
            found54=nrfclaw_ninalink_external_get_event_after(cursor54,&xe54);
            r[0]=found54?1U:0U;
            r[1]=(uint8_t)xe54.event_id;r[2]=(uint8_t)(xe54.event_id>>8);
            r[3]=(uint8_t)(xe54.event_id>>16);r[4]=(uint8_t)(xe54.event_id>>24);
            r[5]=(uint8_t)xe54.node_id;r[6]=(uint8_t)(xe54.node_id>>8);
            r[7]=(uint8_t)(xe54.node_id>>16);r[8]=(uint8_t)(xe54.node_id>>24);
            r[9]=(uint8_t)xe54.capability_id;r[10]=(uint8_t)(xe54.capability_id>>8);
            r[11]=xe54.channel;r[12]=xe54.value_type;
            r[13]=(uint8_t)xe54.sequence;r[14]=(uint8_t)(xe54.sequence>>8);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==34U) {
            nrfclaw_ninalink_external_event_value_t xev54;
            uint32_t event54;bool found54;
            if(n!=5U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            event54=((uint32_t)p[1])|((uint32_t)p[2]<<8)|
                    ((uint32_t)p[3]<<16)|((uint32_t)p[4]<<24);
            memset(&xev54,0,sizeof(xev54));
            found54=nrfclaw_ninalink_external_get_event_value(event54,nrfclaw_rtc_now(),&xev54);
            r[0]=found54?1U:0U;
            r[1]=(uint8_t)xev54.raw_value;r[2]=(uint8_t)(xev54.raw_value>>8);
            r[3]=(uint8_t)(xev54.raw_value>>16);r[4]=(uint8_t)(xev54.raw_value>>24);
            r[5]=(uint8_t)xev54.age_s;r[6]=(uint8_t)(xev54.age_s>>8);
            return reply(op,seq,NDP_OK,r,7,out,ol);
        }

        if(p[0]==35U) {
            nrfclaw_ninalink_external_descriptor_t xd54;
            uint16_t cap54;
            if(n!=4U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            cap54=(uint16_t)p[1]|((uint16_t)p[2]<<8);
            nrfclaw_ninalink_external_get_descriptor(cap54,p[3],&xd54);
            r[0]=xd54.known?1U:0U;r[1]=xd54.kind;r[2]=xd54.value_type;
            r[3]=(uint8_t)xd54.scale10;r[4]=xd54.unit;r[5]=xd54.behavior_flags;
            return reply(op,seq,NDP_OK,r,6,out,ol);
        }
        /* B5.5 External Change Notification / Subscription v1. */
        if(p[0]==36U) {
            nrfclaw_ninalink_change_status_t xs55;
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_external_subscription_get_status(&xs55);
            r[0]=xs55.schema_version;r[1]=xs55.mask;r[2]=xs55.pending_flags;
            r[3]=(uint8_t)xs55.state_revision;
            r[4]=(uint8_t)(xs55.state_revision>>8);
            r[5]=(uint8_t)(xs55.state_revision>>16);
            r[6]=(uint8_t)(xs55.state_revision>>24);
            r[7]=(uint8_t)xs55.event_revision;
            r[8]=(uint8_t)(xs55.event_revision>>8);
            r[9]=(uint8_t)(xs55.event_revision>>16);
            r[10]=(uint8_t)(xs55.event_revision>>24);
            r[11]=(uint8_t)xs55.newest_event_id;
            r[12]=(uint8_t)(xs55.newest_event_id>>8);
            r[13]=(uint8_t)(xs55.newest_event_id>>16);
            r[14]=(uint8_t)(xs55.newest_event_id>>24);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        if(p[0]==37U) {
            nrfclaw_ninalink_change_status_t xs55;
            if(n!=2U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            if((p[1]&~NRFCLAW_NINALINK_CHANGE_ALL)!=0U)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            nrfclaw_ninalink_external_subscription_set(p[1]);
            nrfclaw_ninalink_external_subscription_get_status(&xs55);
            r[0]=xs55.schema_version;r[1]=xs55.mask;r[2]=xs55.pending_flags;
            r[3]=(uint8_t)xs55.state_revision;
            r[4]=(uint8_t)(xs55.state_revision>>8);
            r[5]=(uint8_t)(xs55.state_revision>>16);
            r[6]=(uint8_t)(xs55.state_revision>>24);
            r[7]=(uint8_t)xs55.event_revision;
            r[8]=(uint8_t)(xs55.event_revision>>8);
            r[9]=(uint8_t)(xs55.event_revision>>16);
            r[10]=(uint8_t)(xs55.event_revision>>24);
            r[11]=(uint8_t)xs55.newest_event_id;
            r[12]=(uint8_t)(xs55.newest_event_id>>8);
            r[13]=(uint8_t)(xs55.newest_event_id>>16);
            r[14]=(uint8_t)(xs55.newest_event_id>>24);
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }


        /* B7.2 External Capability Inventory. */
        if(p[0]==40U) {
            uint32_t node_id;
            nrfclaw_ninalink_cap_inventory_status_t ci72;
            if(n!=5U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            node_id=(uint32_t)p[1] |
                    ((uint32_t)p[2]<<8) |
                    ((uint32_t)p[3]<<16) |
                    ((uint32_t)p[4]<<24);
            memset(&ci72,0,sizeof(ci72));
            if(!nrfclaw_ninalink_cap_inventory_get_status(node_id,&ci72)) {
                memset(r,0,6U);
                r[5]=NRFCLAW_NINALINK_CAP_INVENTORY_PER_NODE;
                return reply(op,seq,NDP_OK,r,6,out,ol);
            }
            r[0]=ci72.valid?1U:0U;
            r[1]=ci72.registry_version;
            r[2]=ci72.count;
            r[3]=ci72.complete?1U:0U;
            r[4]=ci72.overflow?1U:0U;
            r[5]=NRFCLAW_NINALINK_CAP_INVENTORY_PER_NODE;
            return reply(op,seq,NDP_OK,r,6,out,ol);
        }
        if(p[0]==41U) {
            uint32_t node_id;
            nrfclaw_ninalink_capability_descriptor_t cd72;
            if(n!=6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            node_id=(uint32_t)p[1] |
                    ((uint32_t)p[2]<<8) |
                    ((uint32_t)p[3]<<16) |
                    ((uint32_t)p[4]<<24);
            if(!nrfclaw_ninalink_cap_inventory_get_at(node_id,p[5],&cd72))
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
            r[0]=(uint8_t)cd72.desc.capability_id;
            r[1]=(uint8_t)(cd72.desc.capability_id>>8);
            r[2]=cd72.desc.channel;
            r[3]=cd72.desc.kind;
            r[4]=cd72.desc.value_type;
            r[5]=(uint8_t)cd72.desc.scale10;
            r[6]=cd72.desc.unit;
            r[7]=cd72.desc.behavior_flags;
            r[8]=cd72.runtime_state_flags;
            return reply(op,seq,NDP_OK,r,9,out,ol);
        }

        if(p[0]==42U) {
            nrfclaw_ninalink_query_endpoint_t ep[2];
            nrfclaw_ninalink_query_status_t qs;
            uint32_t node;
            uint8_t count,i,off=6U,r[2];
            if(n<6U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            node=((uint32_t)p[1])|((uint32_t)p[2]<<8)|((uint32_t)p[3]<<16)|((uint32_t)p[4]<<24);
            count=p[5];
            if(count==0U || count>2U || n!=(uint8_t)(6U+3U*count))
                return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            memset(ep,0,sizeof(ep));
            for(i=0U;i<count;i++) {
                ep[i].capability_id=(uint16_t)p[off]|((uint16_t)p[off+1U]<<8);
                ep[i].channel=p[off+2U];
                off=(uint8_t)(off+3U);
            }
            if(!nrfclaw_ninalink_query_queue(node,ep,count))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);
            nrfclaw_ninalink_query_get_status(&qs);
            r[0]=(uint8_t)qs.command_seq; r[1]=(uint8_t)(qs.command_seq>>8);
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }


        /* B7.6f2l2a: Application/NDP read-only bridge network diagnostics.
         * Network writes remain exclusively on physical NUS NDP 0x62. */
        if(p[0]==52U) {
            uint16_t network_id;
            if(n!=1U) return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);
            nrfclaw_ninalink_bridge_get_status(&bs);
            network_id=nrfclaw_ninalink_network_id();
            r[0]=1U; /* diagnostics contract version */
            r[1]=(uint8_t)network_id;r[2]=(uint8_t)(network_id>>8);
            r[3]=nrfclaw_ninalink_network_has_persisted()?1U:0U;
            r[4]=(uint8_t)bs.foreign_network;r[5]=(uint8_t)(bs.foreign_network>>8);
            return reply(op,seq,NDP_OK,r,6,out,ol);
        }


        /* B7.6f2m6c1 authenticated Application/HA remote semantic write. */
        if(p[0]==54U) {
            uint32_t node;
            uint16_t cap;
            nrfclaw_ninalink_app_dl_status_t app_status;

            if(n!=10U)
                return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

            node=((uint32_t)p[1]) |
                 ((uint32_t)p[2]<<8) |
                 ((uint32_t)p[3]<<16) |
                 ((uint32_t)p[4]<<24);
            cap=(uint16_t)p[5] | ((uint16_t)p[6]<<8);

            if(p[8]!=(uint8_t)NRFCLAW_CAP_VALUE_BOOL &&
               p[8]!=(uint8_t)NRFCLAW_CAP_VALUE_U8 &&
               p[8]!=(uint8_t)NRFCLAW_CAP_VALUE_S8 &&
               p[8]!=(uint8_t)NRFCLAW_CAP_VALUE_ENUM8)
                return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);

            if(!nrfclaw_ninalink_bridge_queue_cap_set(
                    node,cap,p[7],p[8],p[9]))
                return reply(op,seq,NDP_BUSY,0,0,out,ol);

            nrfclaw_ninalink_bridge_get_app_status(&app_status);
            r[0]=(uint8_t)app_status.command_seq;
            r[1]=(uint8_t)(app_status.command_seq>>8);
            return reply(op,seq,NDP_OK,r,2,out,ol);
        }

        /* B7.6f2m6c1 MTU-safe Application CAP_SET status: 15 bytes. */
        if(p[0]==55U) {
            nrfclaw_ninalink_app_dl_status_t app_status;

            if(n!=1U)
                return reply(op,seq,NDP_BAD_LENGTH,0,0,out,ol);

            nrfclaw_ninalink_bridge_get_app_status(&app_status);
            r[0]=1U;
            r[1]=app_status.pending?1U:0U;
            memcpy(&r[2],&app_status.target_node,4);
            r[6]=(uint8_t)app_status.command_seq;
            r[7]=(uint8_t)(app_status.command_seq>>8);
            r[8]=(uint8_t)app_status.capability_id;
            r[9]=(uint8_t)(app_status.capability_id>>8);
            r[10]=app_status.channel;
            r[11]=app_status.value_type;
            r[12]=app_status.requested_value;
            r[13]=app_status.state;
            r[14]=app_status.result;
            return reply(op,seq,NDP_OK,r,15,out,ol);
        }

        return reply(op,seq,NDP_BAD_ARG,0,0,out,ol);
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
