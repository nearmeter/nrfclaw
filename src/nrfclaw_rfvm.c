#include "nrfclaw_rfvm.h"
#include "nrfclaw_rf_profile_registry.h" /* B8.0m0 */

#include <string.h>

#define I(op,a,b,c,imm) \
    {(uint8_t)(op),(uint8_t)(a),(uint8_t)(b),(uint8_t)(c),(int32_t)(imm)}

uint8_t nrfclaw_rfvm_crc8(uint8_t const *data,
                          size_t len,
                          uint8_t poly,
                          uint8_t init)
{
    uint8_t crc = init;

    if (!data) {
        return crc;
    }

    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            if ((crc & 0x80U) != 0U) {
                crc = (uint8_t)((crc << 1) ^ poly);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

static bool range_ok(size_t len, size_t off, size_t count)
{
    return off <= len && count <= (len - off);
}

bool nrfclaw_rfvm_get(nrfclaw_rfvm_result_t const *result,
                      uint8_t field_id,
                      int32_t *value)
{
    if (!result || !value) {
        return false;
    }

    for (uint8_t i = 0U; i < result->field_count; ++i) {
        if (result->field[i].field_id == field_id) {
            *value = result->field[i].value;
            return true;
        }
    }

    return false;
}

static uint16_t
rfvm_crc16_reflect_zpad(uint8_t const *data,
                        size_t len,
                        uint8_t zero_pad,
                        uint16_t poly)
{
    uint16_t crc=0U;
    size_t i;
    uint8_t bit;
    uint8_t pad;

    if(!data)
        return 0U;

    for(i=0U;i<len;++i){
        crc^=data[i];
        for(bit=0U;bit<8U;++bit)
            crc=(crc&1U)?(uint16_t)((crc>>1)^poly):
                         (uint16_t)(crc>>1);
    }

    for(pad=0U;pad<zero_pad;++pad){
        for(bit=0U;bit<8U;++bit)
            crc=(crc&1U)?(uint16_t)((crc>>1)^poly):
                         (uint16_t)(crc>>1);
    }

    return crc;
}

static uint16_t
rfvm_crc16_msb(uint8_t const *data,
               size_t len,
               uint16_t poly,
               uint16_t init)
{
    uint16_t crc=init;
    size_t i;
    uint8_t bit;

    if(!data)
        return 0U;

    for(i=0U;i<len;++i){
        crc^=(uint16_t)data[i]<<8;
        for(bit=0U;bit<8U;++bit)
            crc=(crc&0x8000U)?
                (uint16_t)((crc<<1)^poly):
                (uint16_t)(crc<<1);
    }
    return crc;
}

static uint16_t
rfvm_wmbus_crc16(uint8_t const *data,size_t len)
{
    uint16_t crc=0U;
    size_t i;
    uint8_t bit;

    if(!data)
        return 0U;

    for(i=0U;i<len;++i){
        crc^=(uint16_t)data[i]<<8;
        for(bit=0U;bit<8U;++bit)
            crc=(crc&0x8000U)?
                (uint16_t)((crc<<1)^0x3D65U):
                (uint16_t)(crc<<1);
    }
    return (uint16_t)~crc;
}

static bool
rfvm_wmbus_crc_at(uint8_t const *data,size_t count,size_t crc_off,size_t len)
{
    uint16_t want;
    uint16_t got;

    if(!data || crc_off+2U>len || count>crc_off)
        return false;

    want=rfvm_wmbus_crc16(data,count);
    got=(uint16_t)(((uint16_t)data[crc_off]<<8)|data[crc_off+1U]);
    return want==got;
}

static bool
rfvm_wmbus_b_valid(uint8_t const *payload,size_t len)
{
    size_t L;
    size_t first_crc_off;

    if(!payload || len<13U || len>256U)
        return false;

    L=payload[0];
    if(L<12U || len!=L+1U)
        return false;

    first_crc_off=((L-1U)<126U)?(L-1U):126U;
    if(!rfvm_wmbus_crc_at(payload,first_crc_off,first_crc_off,len))
        return false;

    /*
     * Mirror rtl_433 Format-B second-block rule exactly.
     * L_OFFSET=127, second data begins at 128, and is validated
     * only when L>129.
     */
    if(L>129U){
        size_t count=L-129U;
        size_t off=128U;
        if(off+count+2U>len)
            return false;
        if(!rfvm_wmbus_crc_at(
                &payload[off],count,count,count+2U))
            return false;
    }

    return true;
}

/* B8.1k6j: minimal controlled W-MBus DIF/VIF semantic subset. */
static bool rfvm_emit_field(nrfclaw_rfvm_result_t *out,uint8_t field_id,int32_t value)
{
    uint8_t i;
    if(!out || field_id==0U) return false;
    for(i=0U;i<out->field_count;++i){
        if(out->field[i].field_id==field_id){out->field[i].value=value;return true;}
    }
    if(out->field_count>=NRFCLAW_RFVM_MAX_FIELDS) return false;
    out->field[out->field_count].field_id=field_id;
    out->field[out->field_count].value=value;
    ++out->field_count;
    return true;
}


static bool rfvm_wmbus_bcd_le(uint8_t const *p,
                              uint8_t bytes,
                              int32_t *value)
{
    int64_t v=0;
    int64_t mul=1;
    uint8_t i;

    if(!p || !value || bytes==0U || bytes>6U)
        return false;

    for(i=0U;i<bytes;++i){
        uint8_t lo=(uint8_t)(p[i]&0x0FU);
        uint8_t hi=(uint8_t)(p[i]>>4);
        if(lo>9U || hi>9U)
            return false;
        v+=(int64_t)lo*mul;
        mul*=10;
        v+=(int64_t)hi*mul;
        mul*=10;
        if(v>INT32_MAX)
            return false;
    }

    *value=(int32_t)v;
    return true;
}

static bool rfvm_wmbus_data_value(uint8_t const *payload,
                                  size_t pos,
                                  size_t end,
                                  uint8_t coding,
                                  size_t *consumed,
                                  bool *numeric,
                                  int32_t *value)
{
    size_t n=0U;
    uint32_t u=0U;

    if(!payload || !consumed || !numeric || !value || pos>end)
        return false;

    *consumed=0U;
    *numeric=false;
    *value=0;

    switch(coding){
    case 0x0U:
        *numeric=true;
        return true;

    case 0x1U:
        n=1U;
        if(n>end-pos)return false;
        *value=(int8_t)payload[pos];
        *numeric=true;
        break;

    case 0x2U:
        n=2U;
        if(n>end-pos)return false;
        u=(uint32_t)payload[pos] |
          ((uint32_t)payload[pos+1U]<<8);
        *value=(int16_t)u;
        *numeric=true;
        break;

    case 0x3U:
        n=3U;
        if(n>end-pos)return false;
        u=(uint32_t)payload[pos] |
          ((uint32_t)payload[pos+1U]<<8) |
          ((uint32_t)payload[pos+2U]<<16);
        if((u&0x00800000UL)!=0U)
            u|=0xFF000000UL;
        *value=(int32_t)u;
        *numeric=true;
        break;

    case 0x4U:
        n=4U;
        if(n>end-pos)return false;
        u=(uint32_t)payload[pos] |
          ((uint32_t)payload[pos+1U]<<8) |
          ((uint32_t)payload[pos+2U]<<16) |
          ((uint32_t)payload[pos+3U]<<24);
        *value=(int32_t)u;
        *numeric=true;
        break;

    case 0x5U:
        n=4U;
        if(n>end-pos)return false;
        break;

    case 0x6U:
        n=6U;
        if(n>end-pos)return false;
        break;

    case 0x7U:
        n=8U;
        if(n>end-pos)return false;
        break;

    case 0x8U:
        n=0U;
        break;

    case 0x9U:
        n=1U;
        if(n>end-pos)return false;
        if(!rfvm_wmbus_bcd_le(&payload[pos],1U,value))return false;
        *numeric=true;
        break;

    case 0xAU:
        n=2U;
        if(n>end-pos)return false;
        if(!rfvm_wmbus_bcd_le(&payload[pos],2U,value))return false;
        *numeric=true;
        break;

    case 0xBU:
        n=3U;
        if(n>end-pos)return false;
        if(!rfvm_wmbus_bcd_le(&payload[pos],3U,value))return false;
        *numeric=true;
        break;

    case 0xCU:
        n=4U;
        if(n>end-pos)return false;
        if(!rfvm_wmbus_bcd_le(&payload[pos],4U,value))return false;
        *numeric=true;
        break;

    case 0xDU:{
        uint8_t lvar;
        size_t body;

        if(pos>=end)return false;
        lvar=payload[pos];

        if(lvar<=0xBFU)
            body=(size_t)lvar;
        else if(lvar<=0xCFU)
            body=(size_t)(lvar-0xC0U);
        else if(lvar<=0xDFU)
            body=(size_t)(lvar-0xD0U);
        else if(lvar<=0xEFU)
            body=(size_t)(lvar-0xE0U);
        else if(lvar<=0xFAU)
            body=(size_t)(lvar-0xF0U);
        else
            return false;

        n=1U+body;
        if(n>end-pos)return false;
        break;
    }

    case 0xEU:
        n=6U;
        if(n>end-pos)return false;
        if(rfvm_wmbus_bcd_le(&payload[pos],6U,value))
            *numeric=true;
        break;

    default:
        return false;
    }

    *consumed=n;
    return true;
}

static bool rfvm_wmbus_mul10_checked(int32_t value,
                                     uint8_t power,
                                     int32_t *out)
{
    int64_t v=value;
    uint8_t i;

    if(!out)
        return false;

    for(i=0U;i<power;++i){
        v*=10;
        if(v<INT32_MIN || v>INT32_MAX)
            return false;
    }

    *out=(int32_t)v;
    return true;
}

static bool rfvm_wmbus_emit_once(nrfclaw_rfvm_result_t *out,
                                 uint8_t field_id,
                                 int32_t value)
{
    int32_t ignored;

    if(nrfclaw_rfvm_get(out,field_id,&ignored))
        return true;

    return rfvm_emit_field(out,field_id,value);
}

/* B8.1k6k — standard DIF/VIF traversal for CI=0x78. */
static bool rfvm_wmbus_difvif(uint8_t const *payload,
                              size_t len,
                              nrfclaw_rfvm_result_t *out)
{
    size_t pos;
    size_t end;
    uint8_t l;

    if(!payload || !out || len<13U)
        return false;

    l=payload[0];
    if((size_t)l+1U!=len || l<12U)
        return false;

    end=(l-1U)<126U?(size_t)(l-1U):126U;

    if(end<=10U)
        return true;

    if(payload[10]!=0x78U)
        return true;

    pos=11U;

    while(pos<end){
        uint8_t dif;
        uint8_t coding;
        uint8_t vif;
        uint8_t vif_base;
        uint8_t ext_count=0U;
        bool current;
        bool has_vife=false;
        bool numeric=false;
        size_t consumed=0U;
        int32_t value=0;

        if(payload[pos]==0x2FU){
            ++pos;
            continue;
        }

        dif=payload[pos++];

        if((dif&0x0FU)==0x0FU)
            break;

        coding=(uint8_t)(dif&0x0FU);
        current=((dif&0x30U)==0U) && ((dif&0x40U)==0U);

        if((dif&0x80U)!=0U){
            uint8_t more=1U;
            while(more){
                uint8_t dife;
                if(pos>=end || ext_count>=10U)
                    return false;
                dife=payload[pos++];
                ++ext_count;
                more=(uint8_t)((dife&0x80U)!=0U);
                if((dife&0x7FU)!=0U)
                    current=false;
            }
        }

        if(pos>=end)
            return false;

        vif=payload[pos++];
        vif_base=(uint8_t)(vif&0x7FU);

        if((vif&0x80U)!=0U){
            uint8_t more=1U;
            ext_count=0U;
            has_vife=true;
            while(more){
                uint8_t vife;
                if(pos>=end || ext_count>=10U)
                    return false;
                vife=payload[pos++];
                ++ext_count;
                more=(uint8_t)((vife&0x80U)!=0U);
            }
        }

        if(!rfvm_wmbus_data_value(
                payload,pos,end,coding,&consumed,&numeric,&value))
            return false;

        if(current && !has_vife && numeric){
            if((vif_base&0xF8U)==0x10U && value>=0){
                int32_t ml;
                if(rfvm_wmbus_mul10_checked(
                        value,(uint8_t)(vif_base&0x07U),&ml)){
                    if(!rfvm_wmbus_emit_once(
                            out,NRFCLAW_RF_FIELD_WMBUS_VOLUME_ML,ml))
                        return false;
                }
            }else if((vif_base&0xFCU)==0x64U){
                uint8_t n=(uint8_t)(vif_base&0x03U);
                int32_t centic=0;
                bool ok=true;

                if(n==0U){
                    centic=(value>=0)?
                        (value+5)/10:
                        (value-5)/10;
                }else{
                    ok=rfvm_wmbus_mul10_checked(
                        value,(uint8_t)(n-1U),&centic);
                }

                if(ok && centic>=INT16_MIN && centic<=INT16_MAX){
                    if(!rfvm_wmbus_emit_once(
                            out,
                            NRFCLAW_RF_FIELD_WMBUS_TEMPERATURE_CENTIC,
                            centic))
                        return false;
                }
            }
        }

        pos+=consumed;
    }

    return true;
}




bool nrfclaw_rfvm_run(nrfclaw_rfvm_profile_t const *profile,
                      uint8_t const *payload,
                      size_t len,
                      nrfclaw_rfvm_result_t *out)
{
    if (!profile || !profile->program || !payload || !out) {
        return false;
    }

    if (profile->expected_len != 0U &&
        len != profile->expected_len) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    for (nrfclaw_rfvm_insn_t const *p = profile->program; ; ++p) {
        switch ((nrfclaw_rfvm_opcode_t)p->op) {
        case NRFCLAW_RFVM_OP_END:
            return true;

        case NRFCLAW_RFVM_OP_MATCH8:
            if (!range_ok(len, p->a, 1U) ||
                payload[p->a] != (uint8_t)p->imm) {
                return false;
            }
            break;

        case NRFCLAW_RFVM_OP_MATCH_MASK8_ANY3:{
  uint8_t v,x0,x1,x2;
  if(!range_ok(len,p->a,1U))return false;
  v=(uint8_t)(payload[p->a]&p->b);
  x0=p->c;
  x1=(uint8_t)((uint32_t)p->imm&0xFFU);
  x2=(uint8_t)(((uint32_t)p->imm>>8)&0xFFU);
  if(v!=x0 && v!=x1 && v!=x2)return false;
  break;
 }
 case NRFCLAW_RFVM_OP_CRC8_ZERO:
            if (!range_ok(len, 0U, p->a) ||
                nrfclaw_rfvm_crc8(payload, p->a, p->b, p->c) != 0U) {
                return false;
            }
            break;

        case NRFCLAW_RFVM_OP_CRC8_ZERO_ANY3:{
  uint8_t i0=p->c;
  uint8_t i1=(uint8_t)((uint32_t)p->imm & 0xFFU);
  uint8_t i2=(uint8_t)(((uint32_t)p->imm >> 8) & 0xFFU);
  if(!range_ok(len,0U,p->a))return false;
  if(nrfclaw_rfvm_crc8(payload,p->a,p->b,i0)!=0U &&
     nrfclaw_rfvm_crc8(payload,p->a,p->b,i1)!=0U &&
     nrfclaw_rfvm_crc8(payload,p->a,p->b,i2)!=0U)return false;
  break;
 }
 case NRFCLAW_RFVM_OP_CRC8_EQ:
            if (!range_ok(len, 0U, p->a) ||
                !range_ok(len, p->c, 1U) ||
                nrfclaw_rfvm_crc8(payload, p->a, p->b, 0U) != payload[p->c]) {
                return false;
            }
            break;

        case NRFCLAW_RFVM_OP_SUM8_EQ: {
            uint8_t sum = 0U;

            if (!range_ok(len, 0U, p->a) ||
                !range_ok(len, p->b, 1U)) {
                return false;
            }

            for (uint8_t i = 0U; i < p->a; ++i) {
                sum = (uint8_t)(sum + payload[i]);
            }

            if (sum != payload[p->b]) {
                return false;
            }
            break;
        }

        case NRFCLAW_RFVM_OP_LOAD_U8:
            if (p->a >= NRFCLAW_RFVM_REGS ||
                !range_ok(len, p->b, 1U)) {
                return false;
            }
            out->reg[p->a] = payload[p->b];
            break;

        case NRFCLAW_RFVM_OP_LOAD_BE16:
            if (p->a >= NRFCLAW_RFVM_REGS ||
                !range_ok(len, p->b, 2U)) {
                return false;
            }
            out->reg[p->a] =
                ((int32_t)payload[p->b] << 8) |
                payload[p->b + 1U];
            break;

        case NRFCLAW_RFVM_OP_LOAD_BE24:
            if (p->a >= NRFCLAW_RFVM_REGS ||
                !range_ok(len, p->b, 3U)) {
                return false;
            }
            out->reg[p->a] =
                ((int32_t)payload[p->b] << 16) |
                ((int32_t)payload[p->b + 1U] << 8) |
                payload[p->b + 2U];
            break;

        case NRFCLAW_RFVM_OP_AND_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] &= p->imm;
            break;

        case NRFCLAW_RFVM_OP_SHR_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] >>= (uint8_t)p->imm;
            break;

        case NRFCLAW_RFVM_OP_SHL_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] <<= (uint8_t)p->imm;
            break;

        case NRFCLAW_RFVM_OP_ADD_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] += p->imm;
            break;

        case NRFCLAW_RFVM_OP_MUL_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] *= p->imm;
            break;

        case NRFCLAW_RFVM_OP_DIV_IMM:
  if(p->a>=NRFCLAW_RFVM_REGS || p->imm==0)return false;
  out->reg[p->a]/=p->imm;
  break;
 case NRFCLAW_RFVM_OP_XOR_IMM:
            if (p->a >= NRFCLAW_RFVM_REGS) {
                return false;
            }
            out->reg[p->a] ^= p->imm;
            break;

        case NRFCLAW_RFVM_OP_EQ_IMM:
  if(p->a>=NRFCLAW_RFVM_REGS)return false;
  out->reg[p->a]=(out->reg[p->a]==p->imm)?1:0;
  break;
case NRFCLAW_RFVM_OP_SIGN_MAG_IMM:{
  uint32_t mask,v;
  if(p->a>=NRFCLAW_RFVM_REGS || p->imm<=0)return false;
  mask=(uint32_t)p->imm;
  v=(uint32_t)out->reg[p->a];
  if((v&mask)!=0U)
    out->reg[p->a]=-(int32_t)(v&~mask);
  break;
 }
case NRFCLAW_RFVM_OP_SKIP_IF_NE_IMM:
  if(p->a>=NRFCLAW_RFVM_REGS)return false;
  if(out->reg[p->a]!=p->imm)
    p+=p->b;
  break;
 case NRFCLAW_RFVM_OP_STORE_IF_NE_IMM:
  if(p->a>=NRFCLAW_RFVM_REGS || p->b==0U)return false;
  if(out->reg[p->a]==p->imm)break;
  if(out->field_count>=NRFCLAW_RFVM_MAX_FIELDS)return false;
  out->field[out->field_count].field_id=p->b;
  out->field[out->field_count].value=out->reg[p->a];
  ++out->field_count;
  break;
case NRFCLAW_RFVM_OP_STORE:
            if (p->a >= NRFCLAW_RFVM_REGS ||
                p->b == 0U ||
                out->field_count >= NRFCLAW_RFVM_MAX_FIELDS) {
                return false;
            }
            out->field[out->field_count].field_id = p->b;
            out->field[out->field_count].value = out->reg[p->a];
            ++out->field_count;
            break;

        case NRFCLAW_RFVM_OP_CRC16_REFLECT_ZPAD:
            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,0U,p->b))
                return false;
            out->reg[p->a]=(int32_t)rfvm_crc16_reflect_zpad(
                payload,p->b,p->c,(uint16_t)p->imm);
            break;

        case NRFCLAW_RFVM_OP_MATCH_REG_BE16:
            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,p->b,2U))
                return false;
            if((uint16_t)out->reg[p->a] !=
               (uint16_t)(((uint16_t)payload[p->b]<<8) |
                          payload[p->b+1U]))
                return false;
            break;

        case NRFCLAW_RFVM_OP_CRC8_CALC:
            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,0U,p->b))
                return false;
            out->reg[p->a]=nrfclaw_rfvm_crc8(
                payload,p->b,p->c,(uint8_t)p->imm);
            break;

        case NRFCLAW_RFVM_OP_MATCH_REG_U8:
            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,p->b,1U))
                return false;
            if((uint8_t)out->reg[p->a]!=payload[p->b])
                return false;
            break;

        case NRFCLAW_RFVM_OP_BYTES_AND_ZERO:
            if(!range_ok(len,p->a,1U) ||
               !range_ok(len,p->b,1U))
                return false;
            if((payload[p->a]&payload[p->b])!=0U)
                return false;
            break;

        case NRFCLAW_RFVM_OP_BCD16_LE_X10:{
            uint8_t lo;
            uint8_t hi;
            uint8_t d0,d1,d2,d3;

            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,p->b,2U))
                return false;

            lo=payload[p->b];
            hi=payload[p->b+1U];
            d0=(uint8_t)(lo&0x0FU);
            d1=(uint8_t)(lo>>4);
            d2=(uint8_t)(hi&0x0FU);
            d3=(uint8_t)(hi>>4);

            if(d0>9U || d1>9U || d2>9U || d3>9U)
                return false;

            out->reg[p->a]=
                (int32_t)d3*1000 +
                (int32_t)d2*100 +
                (int32_t)d1*10 +
                (int32_t)d0;
            break;
        }

        case NRFCLAW_RFVM_OP_CRC16_CALC:{
            uint32_t cfg;
            uint16_t poly;
            uint16_t init;
            if(p->a>=NRFCLAW_RFVM_REGS ||
               !range_ok(len,0U,p->b))
                return false;
            cfg=(uint32_t)p->imm;
            poly=(uint16_t)(cfg&0xFFFFU);
            init=(uint16_t)(cfg>>16);
            out->reg[p->a]=(int32_t)rfvm_crc16_msb(
                payload,p->b,poly,init);
            break;
        }

        case NRFCLAW_RFVM_OP_REVERSE8:{
            uint8_t x;
            uint8_t y=0U;
            uint8_t bit;
            if(p->a>=NRFCLAW_RFVM_REGS)
                return false;
            x=(uint8_t)out->reg[p->a];
            for(bit=0U;bit<8U;++bit){
                y=(uint8_t)((y<<1)|(x&1U));
                x>>=1;
            }
            out->reg[p->a]=(int32_t)y;
            break;
        }

        case NRFCLAW_RFVM_OP_WMBUS_B_VALIDATE:
            if(!rfvm_wmbus_b_valid(payload,len))
                return false;
            break;

        case NRFCLAW_RFVM_OP_WMBUS_DIFVIF:
            if(!rfvm_wmbus_difvif(payload,len,out)) return false;
            break;

        case NRFCLAW_RFVM_OP_BCD32_LE:{
            uint32_t v=0U;
            uint32_t mul=1U;
            uint8_t i;

            if(p->a>=NRFCLAW_RFVM_REGS || !range_ok(len,p->b,4U))
                return false;

            for(i=0U;i<4U;++i){
                uint8_t x=payload[(size_t)p->b+i];
                uint32_t d=(uint32_t)(10U*(x>>4)+(x&0x0FU));
                v+=d*mul;
                mul*=100U;
            }

            out->reg[p->a]=(int32_t)v;
            break;
        }

        default:
            return false;
        }
    }
}


nrfclaw_rfvm_profile_t const *
nrfclaw_rfvm_profile(uint8_t profile_id)
{
    return nrfclaw_rf_profile_registry_rfvm(profile_id);
}
