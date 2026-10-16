#include "nrfclaw_rf_ninalink.h"

#include "nrfclaw_rf_capability.h"
#include "nrfclaw_ninalink_cap_inventory.h"
#include "nrfclaw_ninalink_state_cache.h"
#include "nrfclaw_ninalink_node_registry.h"
#include "nrfclaw_ninalink_msg.h"
#include "nrfclaw_capability.h"

#include <string.h>

static nrfclaw_rf_ninalink_status_t m_status;

static bool get_device_id(uint32_t *id)
{
    nrfclaw_rf_capability_entry_t e;
    if(!id) return false;
    if(!nrfclaw_rf_capability_get(NRFCLAW_RF_SEMCAP_DEVICE_ID,0U,&e))
        return false;
    if(e.value.type!=NRFCLAW_CAP_VALUE_U32)
        return false;
    *id=e.value.v.u32;
    return true;
}

static uint32_t virtual_node_id(uint8_t profile_id,uint32_t sensor_id)
{
    return 0xF8000000UL |
           ((uint32_t)(profile_id & 0xFFU) << 16) |
           (sensor_id & 0xFFFFUL);
}

bool nrfclaw_rf_ninalink_publish(uint8_t profile_id,uint32_t now_s)
{
    uint32_t sensor_id;
    uint32_t node_id;
    uint8_t count;
    nrfclaw_ninalink_capability_descriptor_t desc[NRFCLAW_RF_CAP_MAX];
    nrfclaw_ninalink_value_entry_t values[NRFCLAW_RF_CAP_MAX];

    if(!get_device_id(&sensor_id))
        return false;

    count=nrfclaw_rf_capability_count();
    if(count==0U || count>NRFCLAW_RF_CAP_MAX)
        return false;

    memset(desc,0,sizeof(desc));
    memset(values,0,sizeof(values));

    for(uint8_t i=0U;i<count;++i){
        nrfclaw_rf_capability_entry_t e;
        if(!nrfclaw_rf_capability_at(i,&e))
            return false;

        desc[i].desc=e.desc;
        desc[i].runtime_state_flags=
            NRFCLAW_CAP_STATE_SUPPORTED |
            NRFCLAW_CAP_STATE_PRESENT |
            NRFCLAW_CAP_STATE_ENABLED;

        values[i].capability_id=e.desc.capability_id;
        values[i].channel=e.desc.channel;
        values[i].value=e.value;
    }

    node_id=virtual_node_id(profile_id,sensor_id);

    /* B8.0j2 HA-visible virtual node registration.
     * The external HA model enumerates the node registry, while inventory/state
     * are read from the existing capability inventory and state cache.
     */
    {
        nrfclaw_ninalink_frame_t frame;
        nrfclaw_ninalink_node_discovery_t discovery;

        memset(&frame,0,sizeof(frame));
        frame.node_id=node_id;
        frame.network_id=0U;
        frame.sequence=1U;
        frame.message_type=NRFCLAW_NINALINK_MSG_CAP_REPORT;

        nrfclaw_ninalink_node_registry_observe(
            &frame,
            0,
            0,
            now_s
        );

        memset(&discovery,0,sizeof(discovery));
        discovery.state=NRFCLAW_NINALINK_NODE_DISC_READY;
        discovery.capability_count=count;
        discovery.command_count=0U;
        discovery.capability_registry_version=
            NRFCLAW_CAPABILITY_REGISTRY_VERSION;
        discovery.command_registry_version=0U;
        discovery.last_error=NRFCLAW_NINALINK_NODE_DISC_ERR_NONE;

        if(!nrfclaw_ninalink_node_registry_discovery_set(
                node_id,
                &discovery))
            return false;
    }

    /* Store inventory using the existing NinaLink page semantics. */
    {
        uint8_t page=0U;
        uint8_t at=0U;
        while(at<count){
            uint8_t n=(uint8_t)(count-at);
            bool more;
            if(n>NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS)
                n=NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS;
            more=(uint8_t)(at+n)<count;

            if(!nrfclaw_ninalink_cap_inventory_store_page(
                    node_id,
                    NRFCLAW_CAPABILITY_REGISTRY_VERSION,
                    page,
                    &desc[at],
                    n,
                    more))
                return false;

            at=(uint8_t)(at+n);
            ++page;
        }
    }

    if(!nrfclaw_ninalink_state_cache_upsert_query_values(
            node_id,values,count,now_s))
        return false;

    memset(&m_status,0,sizeof(m_status));
    m_status.valid=true;
    m_status.profile_id=profile_id;
    m_status.node_id=node_id;
    m_status.capability_count=count;
    m_status.cached_value_count=count;
    return true;
}

bool nrfclaw_rf_ninalink_status(nrfclaw_rf_ninalink_status_t *out)
{
    if(!out || !m_status.valid)
        return false;
    *out=m_status;
    return true;
}
