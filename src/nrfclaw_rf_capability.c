#include "nrfclaw_rf_capability.h"
#include "nrfclaw_rf_profile_registry.h" /* B8.0m2 */
#include <string.h>

static nrfclaw_rf_capability_entry_t m_entry[NRFCLAW_RF_CAP_MAX];
static uint8_t m_count;

static bool append(uint16_t id,uint8_t channel,uint8_t kind,uint8_t type,
                   int8_t scale10,uint8_t unit,int32_t value)
{
    nrfclaw_rf_capability_entry_t *e;
    if(m_count>=NRFCLAW_RF_CAP_MAX) return false;
    e=&m_entry[m_count++];
    memset(e,0,sizeof(*e));
    e->desc.capability_id=id;e->desc.channel=channel;e->desc.kind=kind;
    e->desc.value_type=type;e->desc.scale10=scale10;e->desc.unit=unit;
    e->desc.behavior_flags=NRFCLAW_CAP_BEHAVIOR_READABLE|NRFCLAW_CAP_BEHAVIOR_REPORTABLE|NRFCLAW_CAP_BEHAVIOR_RETAINED;
    e->value.type=type;
    switch(type){
    case NRFCLAW_CAP_VALUE_BOOL:e->value.v.boolean=value!=0;break;
    case NRFCLAW_CAP_VALUE_U8:
    case NRFCLAW_CAP_VALUE_ENUM8:e->value.v.u8=(uint8_t)value;break;
    case NRFCLAW_CAP_VALUE_S8:e->value.v.s8=(int8_t)value;break;
    case NRFCLAW_CAP_VALUE_U16:e->value.v.u16=(uint16_t)value;break;
    case NRFCLAW_CAP_VALUE_S16:e->value.v.s16=(int16_t)value;break;
    case NRFCLAW_CAP_VALUE_U32:e->value.v.u32=(uint32_t)value;break;
    case NRFCLAW_CAP_VALUE_S32:e->value.v.s32=value;break;
    default:--m_count;memset(e,0,sizeof(*e));return false;
    }
    return true;
}

static bool field(nrfclaw_rfvm_result_t const *r,uint8_t id,int32_t *v)
{
    return nrfclaw_rfvm_get(r,id,v);
}



void nrfclaw_rf_capability_clear(void)
{
    memset(m_entry,0,sizeof(m_entry));
    m_count=0U;
}

bool nrfclaw_rf_capability_publish(uint8_t profile_id,
                                   nrfclaw_rfvm_result_t const *d)
{
    int32_t v;
if(!d || !nrfclaw_rf_profile_registry_runtime_ready(profile_id)) return false;
    nrfclaw_rf_capability_clear();
if(field(d,NRFCLAW_RF_FIELD_ID,&v) &&
       !append(NRFCLAW_RF_SEMCAP_DEVICE_ID,0U,NRFCLAW_CAP_KIND_STATUS,
               NRFCLAW_CAP_VALUE_U32,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_CHANNEL,&v) &&
       !append(NRFCLAW_RF_SEMCAP_CHANNEL,0U,NRFCLAW_CAP_KIND_STATUS,
               NRFCLAW_CAP_VALUE_U8,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_BATTERY_OK,&v) &&
       !append(NRFCLAW_RF_SEMCAP_BATTERY_OK,0U,NRFCLAW_CAP_KIND_STATUS,
               NRFCLAW_CAP_VALUE_BOOL,0,NRFCLAW_CAP_UNIT_BOOLEAN,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_TEMPERATURE_X10,&v) &&
       !append(NRFCLAW_SEMCAP_TEMPERATURE,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_S16,-2,NRFCLAW_CAP_UNIT_CELSIUS,v*10)) return false;
    if(field(d,NRFCLAW_RF_FIELD_HUMIDITY,&v) &&
       !append(NRFCLAW_SEMCAP_HUMIDITY,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,-2,NRFCLAW_CAP_UNIT_PERCENT,v*100)) return false;
    if(field(d,NRFCLAW_RF_FIELD_WIND_RAW,&v) &&
       !append(NRFCLAW_RF_SEMCAP_WIND_RAW,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_S32,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_GUST_RAW,&v) &&
       !append(NRFCLAW_RF_SEMCAP_GUST_RAW,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U32,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_WIND_DIR_DEG,&v) &&
       !append(NRFCLAW_RF_SEMCAP_WIND_DIRECTION,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_RAIN_RAW,&v) &&
       !append(NRFCLAW_RF_SEMCAP_RAIN_RAW,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_S32,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_UV_INDEX,&v) &&
       !append(NRFCLAW_RF_SEMCAP_UV_INDEX,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U8,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;

    if(field(d,NRFCLAW_RF_FIELD_BATTERY_MV,&v) &&
       !append(NRFCLAW_RF_SEMCAP_BATTERY_MV,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_MOISTURE,&v) &&
       !append(NRFCLAW_RF_SEMCAP_SOIL_MOISTURE,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,-2,NRFCLAW_CAP_UNIT_PERCENT,v*100)) return false;
    if(field(d,NRFCLAW_RF_FIELD_BOOST,&v) &&
       !append(NRFCLAW_RF_SEMCAP_TX_BOOST,0U,NRFCLAW_CAP_KIND_STATUS,
               NRFCLAW_CAP_VALUE_U8,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;
    if(field(d,NRFCLAW_RF_FIELD_AD_RAW,&v) &&
       !append(NRFCLAW_RF_SEMCAP_AD_RAW,0U,NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,NRFCLAW_CAP_UNIT_NONE,v)) return false;

    if(field(d,NRFCLAW_RF_FIELD_CONDUCTIVITY_US_CM,&v) &&
       !append(NRFCLAW_RF_SEMCAP_CONDUCTIVITY_US_CM,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,NRFCLAW_CAP_UNIT_NONE,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_WIND_SPEED_X100_MS,&v) &&
       !append(NRFCLAW_RF_SEMCAP_WIND_SPEED_M_S,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,-2,
               NRFCLAW_CAP_UNIT_METER_PER_SECOND,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_WIND_DIRECTION_DEG_V2,&v) &&
       !append(NRFCLAW_RF_SEMCAP_WIND_DIRECTION_DEG,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,
               NRFCLAW_CAP_UNIT_NONE,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_BATTERY_PERCENT,&v) &&
       !append(NRFCLAW_SEMCAP_BATTERY_PERCENT,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U8,0,
               NRFCLAW_CAP_UNIT_PERCENT,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_LIGHTNING_COUNT,&v) &&
       !append(NRFCLAW_RF_SEMCAP_LIGHTNING_COUNT,0U,
               NRFCLAW_CAP_KIND_COUNTER,
               NRFCLAW_CAP_VALUE_U16,0,
               NRFCLAW_CAP_UNIT_COUNT,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_LIGHTNING_DISTANCE_KM,&v) &&
       !append(NRFCLAW_RF_SEMCAP_LIGHTNING_DISTANCE_KM,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,0,
               NRFCLAW_CAP_UNIT_NONE,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_WIND_GUST_X100_MS,&v) &&
       !append(NRFCLAW_RF_SEMCAP_WIND_GUST_M_S,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U16,-2,
               NRFCLAW_CAP_UNIT_METER_PER_SECOND,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_RAIN_TOTAL_X10_MM,&v) &&
       !append(NRFCLAW_RF_SEMCAP_RAIN_TOTAL_MM,0U,
               NRFCLAW_CAP_KIND_COUNTER,
               NRFCLAW_CAP_VALUE_U32,-1,
               NRFCLAW_CAP_UNIT_NONE,v))
        return false;

    if(field(d,NRFCLAW_RF_FIELD_ILLUMINANCE_X10_LUX,&v) &&
       !append(NRFCLAW_SEMCAP_ILLUMINANCE,0U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_U32,-1,
               NRFCLAW_CAP_UNIT_LUX,v))
        return false;

    /* B8.1k4: second instance of canonical temperature on channel 1. */
    if(field(d,NRFCLAW_RF_FIELD_TEMPERATURE2_X10,&v) &&
       !append(NRFCLAW_SEMCAP_TEMPERATURE,1U,
               NRFCLAW_CAP_KIND_MEASUREMENT,
               NRFCLAW_CAP_VALUE_S16,-2,
               NRFCLAW_CAP_UNIT_CELSIUS,v*10))
        return false;

    /* B8.1k5 retained diagnostic status; not a false persistent EVENT. */
    if(field(d,NRFCLAW_RF_FIELD_LAST_BUTTON_CODE,&v) &&
       !append(NRFCLAW_RF_SEMCAP_LAST_BUTTON_CODE,0U,
               NRFCLAW_CAP_KIND_STATUS,
               NRFCLAW_CAP_VALUE_ENUM8,0,
               NRFCLAW_CAP_UNIT_NONE,v))
        return false;


    /* B8.1k6k: parser-normalized W-MBus semantics.
     * Volume is mL raw => U32 scale=-3 L.
     * Temperature is centi-C raw => S16 scale=-2 C.
     */
    if(field(d,NRFCLAW_RF_FIELD_WMBUS_VOLUME_ML,&v)){
        if(v<0)
            return false;
        if(!append(NRFCLAW_SEMCAP_VOLUME,0U,
                   NRFCLAW_CAP_KIND_COUNTER,
                   NRFCLAW_CAP_VALUE_U32,-3,
                   NRFCLAW_CAP_UNIT_LITER,v))
            return false;
    }

    if(field(d,NRFCLAW_RF_FIELD_WMBUS_TEMPERATURE_CENTIC,&v)){
        if(v<INT16_MIN || v>INT16_MAX)
            return false;
        if(!append(NRFCLAW_SEMCAP_TEMPERATURE,0U,
                   NRFCLAW_CAP_KIND_MEASUREMENT,
                   NRFCLAW_CAP_VALUE_S16,-2,
                   NRFCLAW_CAP_UNIT_CELSIUS,v))
            return false;
    }

    return m_count!=0U;
}

uint8_t nrfclaw_rf_capability_count(void){ return m_count; }

bool nrfclaw_rf_capability_at(uint8_t index,nrfclaw_rf_capability_entry_t *out)
{
    if(!out || index>=m_count) return false;
    *out=m_entry[index];
    return true;
}

bool nrfclaw_rf_capability_get(uint16_t id,uint8_t channel,
                               nrfclaw_rf_capability_entry_t *out)
{
    if(!out) return false;
    for(uint8_t i=0U;i<m_count;++i){
        if(m_entry[i].desc.capability_id==id &&
           m_entry[i].desc.channel==channel){
            *out=m_entry[i];
            return true;
        }
    }
    return false;
}
