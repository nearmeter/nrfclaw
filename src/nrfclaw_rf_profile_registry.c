#include "nrfclaw_rf_profile_registry.h"

#include <stddef.h>
#include <string.h>

#define I(op,a,b,c,imm) \
    {(uint8_t)(op),(uint8_t)(a),(uint8_t)(b),(uint8_t)(c),(int32_t)(imm)}

static const nrfclaw_rfvm_insn_t vevor_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,0,0,0,0xAA),
    I(NRFCLAW_RFVM_OP_MATCH8,1,0,0,0x00),
    I(NRFCLAW_RFVM_OP_SUM8_EQ,19,19,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,2,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,1,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0F),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_CHANNEL,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,7),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_OK,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,5,0,0),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-500),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,7,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_HUMIDITY,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,8,0,0),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-257),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_RAW,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,10,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_GUST_RAW,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,11,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-257),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_DIR_DEG,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,13,0,0),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-257),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_RAIN_RAW,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,15,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x1F),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_UV_INDEX,0,0),
    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t wh31e_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,0,0,0,0x30),
    I(NRFCLAW_RFVM_OP_CRC8_ZERO,6,0x31,0,0),
    I(NRFCLAW_RFVM_OP_SUM8_EQ,6,6,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,1,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,2,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x70),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_CHANNEL,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,2,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x04),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,2),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_OK,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,2,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x03FF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-400),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_HUMIDITY,0,0),
    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t wh51_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,0,0,0,0x51),
    I(NRFCLAW_RFVM_OP_CRC8_EQ,12,0x31,12,0),
    I(NRFCLAW_RFVM_OP_SUM8_EQ,13,13,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,1,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,5),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,7),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BOOST,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x1F),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,100),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_MV,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,6,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_MOISTURE,0,0),
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,7,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x01FF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_AD_RAW,0,0),
    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t wh52_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,0,0,0,0xA2),
    I(NRFCLAW_RFVM_OP_CRC8_EQ,22,0x31,22,0),
    I(NRFCLAW_RFVM_OP_SUM8_EQ,23,23,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,1,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,5),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,7),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BOOST,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,4,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x1FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-400),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,6,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_MOISTURE,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,8,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFFFF),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,10),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,8),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_CONDUCTIVITY_US_CM,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,20,0,0),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,20),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_MV,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};


static const nrfclaw_rfvm_profile_t rfvm_vevor = {
    NRFCLAW_RF_PROFILE_VEVOR_YT60234, 21U, vevor_program
};
static const nrfclaw_rfvm_profile_t rfvm_wh31e = {
    NRFCLAW_RF_PROFILE_AMBIENT_WH31E, 11U, wh31e_program
};
static const nrfclaw_rfvm_profile_t rfvm_wh51 = {
    NRFCLAW_RF_PROFILE_ECOWITT_WH51, 14U, wh51_program
};

static const nrfclaw_rfvm_profile_t rfvm_wh52 = {
    NRFCLAW_RF_PROFILE_ECOWITT_WH52, 24U, wh52_program
};


#include "nrfclaw_rf_family_promotions.inc"

static const nrfclaw_rf_profile_entry_t profiles[] = {
    {
        NRFCLAW_RF_PROFILE_VEVOR_YT60234,
        "VEVOR YT60234",
        "rtl_433/vevor_7in1",
        NRFCLAW_RF_MODULATION_FSK_PCM,
        NRFCLAW_RF_PROFILE_FLAG_CATALOG |
        NRFCLAW_RF_PROFILE_FLAG_RFVM_READY |
        NRFCLAW_RF_PROFILE_FLAG_RADIO_READY |
        NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY |
        NRFCLAW_RF_PROFILE_FLAG_HARDWARE_VALIDATED,
        915000000UL,11260UL,37000UL,117300UL,
        0x0BU,24U,{0xCAU,0xCAU,0x54U,0x00U},3U,21U,&rfvm_vevor
    },
    {
        NRFCLAW_RF_PROFILE_AMBIENT_WH31E,
        "Ambient WH31E",
        "rtl_433/fineoffset",
        NRFCLAW_RF_MODULATION_FSK_PCM,
        NRFCLAW_RF_PROFILE_FLAG_CATALOG |
        NRFCLAW_RF_PROFILE_FLAG_RFVM_READY |
        NRFCLAW_RF_PROFILE_FLAG_RADIO_READY |
        NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY,
        915000000UL,17857UL,40000UL,117300UL,
        0x0BU,24U,{0x2DU,0xD4U,0x00U,0x00U},2U,11U,&rfvm_wh31e
    },
    {
        NRFCLAW_RF_PROFILE_ECOWITT_WH51,
        "Ecowitt WH51",
        "rtl_433/fineoffset",
        NRFCLAW_RF_MODULATION_FSK_PCM,
        NRFCLAW_RF_PROFILE_FLAG_CATALOG |
        NRFCLAW_RF_PROFILE_FLAG_RFVM_READY |
        NRFCLAW_RF_PROFILE_FLAG_RADIO_READY |
        NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY,
        915000000UL,
        17241UL,
        30000UL,
        93800UL,
        0x13U,
        24U,
        {0x2DU,0xD4U,0x00U,0x00U},
        2U,
        14U,
        &rfvm_wh51
    },
    {
        NRFCLAW_RF_PROFILE_ECOWITT_WH52,
        "Ecowitt WH52",
        "rtl_433/fineoffset_wh52",
        NRFCLAW_RF_MODULATION_FSK_PCM,
        NRFCLAW_RF_PROFILE_FLAG_CATALOG |
        NRFCLAW_RF_PROFILE_FLAG_RFVM_READY |
        NRFCLAW_RF_PROFILE_FLAG_RADIO_READY |
        NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY,
        915000000UL,17241UL,30000UL,93800UL,
        0x13U,24U,{0x2DU,0xD4U,0x00U,0x00U},2U,24U,&rfvm_wh52
    }


#include "nrfclaw_rf_candidate_profiles.inc"
};

uint8_t nrfclaw_rf_profile_registry_count(void)
{
    return (uint8_t)(sizeof(profiles)/sizeof(profiles[0]));
}

const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_at(uint8_t index)
{
    if(index >= nrfclaw_rf_profile_registry_count())
        return NULL;
    return &profiles[index];
}

const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_get(uint8_t profile_id)
{
    uint8_t i;
    for(i=0U;i<nrfclaw_rf_profile_registry_count();++i)
        if(profiles[i].profile_id == profile_id)
            return &profiles[i];
    return NULL;
}

const nrfclaw_rf_profile_entry_t *
nrfclaw_rf_profile_registry_find(const char *name)
{
    uint8_t i;
    if(!name || !name[0])
        return NULL;
    for(i=0U;i<nrfclaw_rf_profile_registry_count();++i)
        if(strcmp(profiles[i].name,name)==0)
            return &profiles[i];
    return NULL;
}

bool nrfclaw_rf_profile_registry_rfvm_ready(uint8_t profile_id)
{
    const nrfclaw_rf_profile_entry_t *p =
        nrfclaw_rf_profile_registry_get(profile_id);
    return p && p->rfvm &&
           (p->flags & NRFCLAW_RF_PROFILE_FLAG_RFVM_READY) != 0U;
}

bool nrfclaw_rf_profile_registry_runtime_ready(uint8_t profile_id)
{
    const nrfclaw_rf_profile_entry_t *p =
        nrfclaw_rf_profile_registry_get(profile_id);
    const uint8_t need =
        NRFCLAW_RF_PROFILE_FLAG_RFVM_READY |
        NRFCLAW_RF_PROFILE_FLAG_RADIO_READY |
        NRFCLAW_RF_PROFILE_FLAG_CAPABILITY_READY;
    return p && p->rfvm && (p->flags & need) == need;
}

const nrfclaw_rfvm_profile_t *
nrfclaw_rf_profile_registry_rfvm(uint8_t profile_id)
{
    const nrfclaw_rf_profile_entry_t *p =
        nrfclaw_rf_profile_registry_get(profile_id);
    if(!p || !(p->flags & NRFCLAW_RF_PROFILE_FLAG_RFVM_READY))
        return NULL;
    return p->rfvm;
}
