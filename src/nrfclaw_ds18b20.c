#include "nrfclaw_ds18b20.h"
#include "nrfclaw_board.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "app_timer.h"
#include "app_error.h"

APP_TIMER_DEF(m_ds18_timer);
static bool m_present,m_busy,m_valid;
static int32_t m_last_mc;
static void ow_release(void){nrf_gpio_cfg_input(P_DS18,NRF_GPIO_PIN_NOPULL);}static void ow_low(void){nrf_gpio_cfg_output(P_DS18);nrf_gpio_pin_clear(P_DS18);}
static bool ow_reset(void){ow_low();nrf_delay_us(500);ow_release();nrf_delay_us(70);bool p=!nrf_gpio_pin_read(P_DS18);nrf_delay_us(410);return p;}
static void ow_write_bit(bool b){ow_low();if(b){nrf_delay_us(6);ow_release();nrf_delay_us(64);}else{nrf_delay_us(60);ow_release();nrf_delay_us(10);}}
static bool ow_read_bit(void){ow_low();nrf_delay_us(3);ow_release();nrf_delay_us(10);bool b=nrf_gpio_pin_read(P_DS18);nrf_delay_us(53);return b;}
static void ow_write_byte(uint8_t v){for(uint8_t i=0;i<8;i++){ow_write_bit((v&1U)!=0);v>>=1;}}
static uint8_t ow_read_byte(void){uint8_t v=0;for(uint8_t i=0;i<8;i++)if(ow_read_bit())v|=(uint8_t)(1U<<i);return v;}
static uint8_t crc8(const uint8_t*d,uint8_t n){uint8_t c=0;for(uint8_t i=0;i<n;i++){uint8_t in=d[i];for(uint8_t b=0;b<8;b++){uint8_t mix=(c^in)&1U;c>>=1;if(mix)c^=0x8CU;in>>=1;}}return c;}
static void timer_handler(void*ctx){(void)ctx;nrfclaw_event_t e={.type=NRFCLAW_EVT_DS18B20_STEP};(void)nrfclaw_event_push_isr(&e);}
bool nrfclaw_ds18b20_probe(void){m_present=ow_reset();ow_release();return m_present;}
void nrfclaw_ds18b20_init(void){ow_release();m_busy=false;m_valid=false;m_last_mc=0;APP_ERROR_CHECK(app_timer_create(&m_ds18_timer,APP_TIMER_MODE_SINGLE_SHOT,timer_handler));m_present=nrfclaw_ds18b20_probe();}
bool nrfclaw_ds18b20_present(void){return m_present;}bool nrfclaw_ds18b20_busy(void){return m_busy;}bool nrfclaw_ds18b20_last_mC(int32_t*v){if(!v||!m_valid)return false;*v=m_last_mc;return true;}
bool nrfclaw_ds18b20_start(void){if(!m_present||m_busy)return false;if(!ow_reset())return false;ow_write_byte(0xCC);ow_write_byte(0x44);m_busy=true;/* default 12-bit conversion worst case */APP_ERROR_CHECK(app_timer_start(m_ds18_timer,APP_TIMER_TICKS(750),0));return true;}
void nrfclaw_ds18b20_on_event(const nrfclaw_event_t*e){if(!e||e->type!=NRFCLAW_EVT_DS18B20_STEP||!m_busy)return;uint8_t s[9];if(!ow_reset()){m_busy=false;return;}ow_write_byte(0xCC);ow_write_byte(0xBE);for(uint8_t i=0;i<9;i++)s[i]=ow_read_byte();ow_release();m_busy=false;if(crc8(s,8)!=s[8])return;int16_t raw=(int16_t)((uint16_t)s[0]|((uint16_t)s[1]<<8));m_last_mc=((int32_t)raw*1000L)/16L;m_valid=true;nrfclaw_event_t done={.type=NRFCLAW_EVT_DS18B20_DONE,.arg0=(uint32_t)m_last_mc};(void)nrfclaw_event_push(&done);}


void nrfclaw_ds18b20_idle_lowpower(void)
{
    if (m_busy) {
        (void)app_timer_stop(m_ds18_timer);
        m_busy=false;
    }
    NRF_GPIO->PIN_CNF[P_DS18] =
        (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
        (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
        (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}
