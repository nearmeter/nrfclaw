#include "nrfclaw_sht.h"

#include "nrfclaw_board.h"
#include "nrfclaw_ds18b20.h"
#include "nrfclaw_ext_i2c.h"
#include "nrfclaw_inputs.h"
#include "nrfclaw_lis2dh12.h"
#include "nrfclaw_serial.h"

#include "nrf_delay.h"

#include <stddef.h>
#include <string.h>

#define SHT2X_ADDR 0x40U
#define SHT3X_ADDR_A 0x44U
#define SHT3X_ADDR_B 0x45U
#define SHT4X_ADDR_C 0x46U
#define SHTC3_ADDR 0x70U

static nrfclaw_ext_i2c_t m_bus;
static nrfclaw_sht_info_t m_info;
static bool m_active;
static bool m_present;

uint8_t nrfclaw_sht_crc8(uint8_t const *data,uint8_t len)
{
    uint8_t crc=0xFFU;
    uint8_t i,j;

    if(!data && len!=0U)
        return 0U;

    for(i=0U;i<len;i++){
        crc^=data[i];
        for(j=0U;j<8U;j++)
            crc=(crc&0x80U)?(uint8_t)((crc<<1U)^0x31U):(uint8_t)(crc<<1U);
    }
    return crc;
}

static bool crc_word_ok(uint8_t const *p)
{
    return nrfclaw_sht_crc8(p,2U)==p[2];
}

static bool crc_byte_ok(uint8_t const *p)
{
    return nrfclaw_sht_crc8(p,1U)==p[1];
}

static bool connector_pin(uint8_t pin)
{
    switch(pin){
        case P_HALL1:
        case P_HALL2:
        case P_FREE_D15:
        case P_FREE_D16:
        case P_FREE_A29:
        case P_FREE_A30:
        case P_FREE_A31:
        case P_DS18:
        case P_LIS_SDA:
            return true;
        default:
            return false;
    }
}

static bool hall_owns_pin(uint8_t pin)
{
    nrfclaw_hall_mode_t mode=nrfclaw_hall_mode();

    if(mode==NRFCLAW_HALL_MODE_DISABLED)
        return false;

    if(mode==NRFCLAW_HALL_MODE_QUADRATURE)
        return pin==P_HALL1 || pin==P_HALL2;

    if(mode==NRFCLAW_HALL_MODE_SINGLE){
        uint8_t hall_pin=(nrfclaw_hall_channel()==2U)?P_HALL2:P_HALL1;
        return pin==hall_pin;
    }

    return false;
}

static bool pin_claimable(uint8_t pin)
{
    if(!connector_pin(pin))
        return false;

    if(hall_owns_pin(pin))
        return false;

    if(nrfclaw_serial_owns_pin(pin))
        return false;

    if(pin==P_DS18 && nrfclaw_ds18b20_present())
        return false;

    if(pin==P_LIS_SDA && nrfclaw_lis2dh12_present())
        return false;

    return true;
}

bool nrfclaw_sht_active(void)
{
    return m_active;
}

bool nrfclaw_sht_present(void)
{
    return m_active && m_present;
}

bool nrfclaw_sht_owns_pin(uint8_t pin)
{
    return m_active && (pin==m_info.scl_pin || pin==m_info.sda_pin);
}

void nrfclaw_sht_info(nrfclaw_sht_info_t *out)
{
    if(out)
        *out=m_info;
}

void nrfclaw_sht_release(void)
{
    nrfclaw_ext_i2c_deinit(&m_bus);
    memset(&m_info,0,sizeof(m_info));
    m_active=false;
    m_present=false;
}

nrfclaw_sht_status_t nrfclaw_sht_configure(uint8_t scl_pin,uint8_t sda_pin)
{
    if(scl_pin==sda_pin || scl_pin>31U || sda_pin>31U)
        return NRFCLAW_SHT_BAD_ARG;

    if(m_active)
        nrfclaw_sht_release();

    if(!pin_claimable(scl_pin) || !pin_claimable(sda_pin))
        return NRFCLAW_SHT_BUSY;

    if(!nrfclaw_ext_i2c_configure(&m_bus,scl_pin,sda_pin))
        return NRFCLAW_SHT_BAD_ARG;

    memset(&m_info,0,sizeof(m_info));
    m_info.scl_pin=scl_pin;
    m_info.sda_pin=sda_pin;
    m_active=true;
    m_present=false;
    return NRFCLAW_SHT_OK;
}

nrfclaw_sht_status_t nrfclaw_sht_configure_default(void)
{
    return nrfclaw_sht_configure((uint8_t)P_SHT_SCL_DEFAULT,
                                 (uint8_t)P_SHT_SDA_DEFAULT);
}

static bool write_cmd8(uint8_t addr,uint8_t cmd)
{
    return nrfclaw_ext_i2c_write(&m_bus,addr,&cmd,1U);
}

static bool write_cmd16(uint8_t addr,uint16_t cmd)
{
    uint8_t b[2]={(uint8_t)(cmd>>8),(uint8_t)cmd};
    return nrfclaw_ext_i2c_write(&m_bus,addr,b,2U);
}

static bool probe_sht4x(uint8_t addr,uint32_t *serial)
{
    uint8_t cmd=0x89U;
    uint8_t r[6];

    if(!nrfclaw_ext_i2c_write(&m_bus,addr,&cmd,1U))
        return false;
    nrf_delay_ms(1U);
    if(!nrfclaw_ext_i2c_read(&m_bus,addr,r,sizeof(r)))
        return false;
    if(!crc_word_ok(&r[0]) || !crc_word_ok(&r[3]))
        return false;

    if(serial)
        *serial=((uint32_t)r[0]<<24)|((uint32_t)r[1]<<16)|
                ((uint32_t)r[3]<<8)|r[4];
    return true;
}

static bool probe_sht3x(uint8_t addr,uint32_t *serial)
{
    uint8_t r[6];

    if(!write_cmd16(addr,0x3780U))
        return false;
    nrf_delay_ms(1U);
    if(!nrfclaw_ext_i2c_read(&m_bus,addr,r,sizeof(r)))
        return false;
    if(!crc_word_ok(&r[0]) || !crc_word_ok(&r[3]))
        return false;

    if(serial)
        *serial=((uint32_t)r[0]<<24)|((uint32_t)r[1]<<16)|
                ((uint32_t)r[3]<<8)|r[4];
    return true;
}

static bool probe_sht2x(uint32_t *serial)
{
    uint8_t cmd1[2]={0xFAU,0x0FU};
    uint8_t cmd2[2]={0xFCU,0xC9U};
    uint8_t a[8],b[6];
    uint8_t i;

    if(!nrfclaw_ext_i2c_write(&m_bus,SHT2X_ADDR,cmd1,sizeof(cmd1)))
        return false;
    if(!nrfclaw_ext_i2c_read(&m_bus,SHT2X_ADDR,a,sizeof(a)))
        return false;

    for(i=0U;i<8U;i+=2U)
        if(!crc_byte_ok(&a[i]))
            return false;

    if(!nrfclaw_ext_i2c_write(&m_bus,SHT2X_ADDR,cmd2,sizeof(cmd2)))
        return false;
    if(!nrfclaw_ext_i2c_read(&m_bus,SHT2X_ADDR,b,sizeof(b)))
        return false;

    if(!crc_word_ok(&b[0]) || !crc_word_ok(&b[3]))
        return false;

    if(serial)
        *serial=((uint32_t)a[0]<<24)|((uint32_t)a[2]<<16)|
                ((uint32_t)a[4]<<8)|a[6];
    return true;
}

static bool probe_shtc3(uint32_t *serial)
{
    uint8_t r[3];

    if(!write_cmd16(SHTC3_ADDR,0x3517U))
        return false;
    nrf_delay_us(300U);

    if(!write_cmd16(SHTC3_ADDR,0xEFC8U)){
        (void)write_cmd16(SHTC3_ADDR,0xB098U);
        return false;
    }

    if(!nrfclaw_ext_i2c_read(&m_bus,SHTC3_ADDR,r,sizeof(r))){
        (void)write_cmd16(SHTC3_ADDR,0xB098U);
        return false;
    }

    (void)write_cmd16(SHTC3_ADDR,0xB098U);

    if(!crc_word_ok(r))
        return false;

    if(serial)
        *serial=((uint32_t)r[0]<<8)|r[1];
    return true;
}

static void detected(nrfclaw_sht_family_t family,uint8_t address,
                     uint32_t serial,bool serial_valid)
{
    m_info.family=family;
    m_info.address=address;
    m_info.serial=serial;
    m_info.serial_valid=serial_valid;
    m_present=true;
}

nrfclaw_sht_status_t nrfclaw_sht_probe(void)
{
    uint32_t serial=0U;

    if(!m_active)
        return NRFCLAW_SHT_BAD_ARG;

    m_present=false;
    m_info.family=NRFCLAW_SHT_FAMILY_NONE;
    m_info.address=0U;
    m_info.serial=0U;
    m_info.serial_valid=false;

    if(probe_shtc3(&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHTC3,SHTC3_ADDR,serial,true);
        return NRFCLAW_SHT_OK;
    }

    if(probe_sht2x(&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT2X,SHT2X_ADDR,serial,true);
        return NRFCLAW_SHT_OK;
    }

    if(probe_sht4x(SHT3X_ADDR_A,&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT4X,SHT3X_ADDR_A,serial,true);
        return NRFCLAW_SHT_OK;
    }
    if(probe_sht3x(SHT3X_ADDR_A,&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT3X,SHT3X_ADDR_A,serial,true);
        return NRFCLAW_SHT_OK;
    }

    if(probe_sht4x(SHT3X_ADDR_B,&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT4X,SHT3X_ADDR_B,serial,true);
        return NRFCLAW_SHT_OK;
    }
    if(probe_sht3x(SHT3X_ADDR_B,&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT3X,SHT3X_ADDR_B,serial,true);
        return NRFCLAW_SHT_OK;
    }

    if(probe_sht4x(SHT4X_ADDR_C,&serial)){
        detected(NRFCLAW_SHT_FAMILY_SHT4X,SHT4X_ADDR_C,serial,true);
        return NRFCLAW_SHT_OK;
    }

    return NRFCLAW_SHT_NOT_FOUND;
}

static uint32_t clamp_rh(int64_t rh)
{
    if(rh<0)
        return 0U;
    if(rh>100000)
        return 100000U;
    return (uint32_t)rh;
}

static bool read_sht4x(nrfclaw_sht_sample_t *out)
{
    uint8_t r[6];
    uint16_t rt,rh;

    if(!write_cmd8(m_info.address,0xFDU))
        return false;
    nrf_delay_ms(10U);
    if(!nrfclaw_ext_i2c_read(&m_bus,m_info.address,r,sizeof(r)))
        return false;
    if(!crc_word_ok(&r[0]) || !crc_word_ok(&r[3]))
        return false;

    rt=((uint16_t)r[0]<<8)|r[1];
    rh=((uint16_t)r[3]<<8)|r[4];
    out->temperature_mC=(int32_t)(-45000LL+(175000LL*rt)/65535LL);
    out->humidity_mpermille=clamp_rh(-6000LL+(125000LL*rh)/65535LL);
    return true;
}

static bool read_sht3x(nrfclaw_sht_sample_t *out)
{
    uint8_t r[6];
    uint16_t rt,rh;

    if(!write_cmd16(m_info.address,0x2400U))
        return false;
    nrf_delay_ms(16U);
    if(!nrfclaw_ext_i2c_read(&m_bus,m_info.address,r,sizeof(r)))
        return false;
    if(!crc_word_ok(&r[0]) || !crc_word_ok(&r[3]))
        return false;

    rt=((uint16_t)r[0]<<8)|r[1];
    rh=((uint16_t)r[3]<<8)|r[4];
    out->temperature_mC=(int32_t)(-45000LL+(175000LL*rt)/65535LL);
    out->humidity_mpermille=clamp_rh((100000LL*rh)/65535LL);
    return true;
}

static bool read_sht2x_word(uint8_t cmd,uint32_t delay_ms,uint16_t *raw)
{
    uint8_t r[3];

    if(!raw)
        return false;
    if(!write_cmd8(SHT2X_ADDR,cmd))
        return false;
    nrf_delay_ms(delay_ms);
    if(!nrfclaw_ext_i2c_read(&m_bus,SHT2X_ADDR,r,sizeof(r)))
        return false;
    if(!crc_word_ok(r))
        return false;

    *raw=((uint16_t)r[0]<<8)|r[1];
    return true;
}

static bool read_sht2x(nrfclaw_sht_sample_t *out)
{
    uint16_t rt,rh;

    if(!read_sht2x_word(0xF3U,90U,&rt))
        return false;
    if(!read_sht2x_word(0xF5U,35U,&rh))
        return false;

    rh&=0xFFFCU;
    out->temperature_mC=(int32_t)(-46850LL+(175720LL*rt)/65536LL);
    out->humidity_mpermille=clamp_rh(-6000LL+(125000LL*rh)/65536LL);
    return true;
}

static bool read_shtc3(nrfclaw_sht_sample_t *out)
{
    uint8_t r[6];
    uint16_t rt,rh;
    bool ok=false;

    if(!write_cmd16(SHTC3_ADDR,0x3517U))
        return false;
    nrf_delay_us(300U);

    if(!write_cmd16(SHTC3_ADDR,0x609CU))
        goto done;
    nrf_delay_ms(2U);

    if(!nrfclaw_ext_i2c_read(&m_bus,SHTC3_ADDR,r,sizeof(r)))
        goto done;
    if(!crc_word_ok(&r[0]) || !crc_word_ok(&r[3]))
        goto done;

    rt=((uint16_t)r[0]<<8)|r[1];
    rh=((uint16_t)r[3]<<8)|r[4];
    out->temperature_mC=(int32_t)(-45000LL+(175000LL*rt)/65535LL);
    out->humidity_mpermille=clamp_rh((100000LL*rh)/65535LL);
    ok=true;

done:
    (void)write_cmd16(SHTC3_ADDR,0xB098U);
    return ok;
}

nrfclaw_sht_status_t nrfclaw_sht_read(nrfclaw_sht_sample_t *out)
{
    bool ok=false;

    if(!out)
        return NRFCLAW_SHT_BAD_ARG;
    if(!m_active || !m_present)
        return NRFCLAW_SHT_NOT_FOUND;

    memset(out,0,sizeof(*out));

    switch(m_info.family){
        case NRFCLAW_SHT_FAMILY_SHT2X:
            ok=read_sht2x(out);
            break;
        case NRFCLAW_SHT_FAMILY_SHT3X:
            ok=read_sht3x(out);
            break;
        case NRFCLAW_SHT_FAMILY_SHT4X:
            ok=read_sht4x(out);
            break;
        case NRFCLAW_SHT_FAMILY_SHTC3:
            ok=read_shtc3(out);
            break;
        default:
            return NRFCLAW_SHT_NOT_FOUND;
    }

    return ok?NRFCLAW_SHT_OK:NRFCLAW_SHT_IO_ERROR;
}
