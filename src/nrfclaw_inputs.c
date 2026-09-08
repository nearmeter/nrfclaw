#include "nrfclaw_inputs.h"
#include "nrfclaw_board.h"
#include "nrf_drv_gpiote.h"
#include "app_error.h"
#include "nrfclaw_event.h"

static volatile int32_t m_pos;
static volatile uint32_t m_count;
static volatile uint8_t m_prev_ab;
static nrfclaw_hall_config_t m_cfg;
static uint32_t m_hall1_init_rc;
static uint32_t m_hall2_init_rc;

static uint32_t single_pin(void) { return (m_cfg.channel==2U)?P_HALL2:P_HALL1; }

static void emit_hall(uint32_t value) {
    if (!m_cfg.emit_events) return;
    nrfclaw_event_t e={0};
    e.type=NRFCLAW_EVT_HALL;
    e.arg0=value;
    (void)nrfclaw_event_push_isr(&e);
}

static void update_quadrature(void) {
    uint8_t a=nrf_gpio_pin_read(P_HALL1)?1U:0U;
    uint8_t b=nrf_gpio_pin_read(P_HALL2)?1U:0U;
    uint8_t ab=(uint8_t)((a<<1)|b);
    static const int8_t lut[16]={0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    int8_t delta=lut[(m_prev_ab<<2)|ab];
    if(delta) {
        m_pos += delta;
        if(m_cfg.count_edges) m_count++;
    }
    m_prev_ab=ab;
}

static void gpiote_handler(nrf_drv_gpiote_pin_t pin,nrf_gpiote_polarity_t action) {
    (void)action;
    if(m_cfg.mode==NRFCLAW_HALL_MODE_SINGLE && pin==single_pin()) {
        if(m_cfg.count_edges) m_count++;
        emit_hall(m_cfg.count_edges ? m_count : (nrf_gpio_pin_read(single_pin())?1U:0U));
        return;
    }
    if(m_cfg.mode==NRFCLAW_HALL_MODE_QUADRATURE && (pin==P_HALL1 || pin==P_HALL2)) {
        update_quadrature();
        emit_hall((uint32_t)m_pos);
        return;
    }
    if(pin==P_BUTTON) {
        nrfclaw_event_t e={0}; e.type=NRFCLAW_EVT_BUTTON;
        (void)nrfclaw_event_push_isr(&e);
    }
}

void nrfclaw_inputs_init(void) {
    if(!nrf_drv_gpiote_is_init()) APP_ERROR_CHECK(nrf_drv_gpiote_init());
    nrf_gpio_cfg_input(P_HALL1,NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(P_HALL2,NRF_GPIO_PIN_PULLDOWN);
    m_cfg.mode=NRFCLAW_HALL_MODE_DISABLED; m_cfg.channel=1U; m_cfg.pullup=false;
    m_cfg.count_edges=false; m_cfg.emit_events=false;
    m_pos=0; m_count=0; m_prev_ab=0; m_hall1_init_rc=NRF_SUCCESS; m_hall2_init_rc=NRF_SUCCESS;
    nrf_drv_gpiote_in_config_t btn=GPIOTE_CONFIG_IN_SENSE_HITOLO(false);
    btn.pull=NRF_GPIO_PIN_PULLUP;
    APP_ERROR_CHECK(nrf_drv_gpiote_in_init(P_BUTTON,&btn,gpiote_handler));
    nrf_drv_gpiote_in_event_enable(P_BUTTON,true);
}

void nrfclaw_hall_disable(void) {
    if(m_cfg.mode!=NRFCLAW_HALL_MODE_DISABLED) {
        if(m_cfg.mode==NRFCLAW_HALL_MODE_SINGLE) {
            uint32_t pin=single_pin();
            nrf_drv_gpiote_in_event_disable(pin);
            nrf_drv_gpiote_in_uninit(pin);
        } else {
            nrf_drv_gpiote_in_event_disable(P_HALL1);
            nrf_drv_gpiote_in_uninit(P_HALL1);
            nrf_drv_gpiote_in_event_disable(P_HALL2);
            nrf_drv_gpiote_in_uninit(P_HALL2);
        }
    }
    nrf_gpio_cfg_input(P_HALL1,NRF_GPIO_PIN_PULLDOWN);
    nrf_gpio_cfg_input(P_HALL2,NRF_GPIO_PIN_PULLDOWN);
    m_cfg.mode=NRFCLAW_HALL_MODE_DISABLED;
}

bool nrfclaw_hall_configure(nrfclaw_hall_config_t const *cfg) {
    if(!cfg || (cfg->mode!=NRFCLAW_HALL_MODE_SINGLE && cfg->mode!=NRFCLAW_HALL_MODE_QUADRATURE))
        return false;
    if(cfg->mode==NRFCLAW_HALL_MODE_SINGLE && cfg->channel!=1U && cfg->channel!=2U)
        return false;
    nrfclaw_hall_disable();
    nrf_drv_gpiote_in_config_t hall=GPIOTE_CONFIG_IN_SENSE_TOGGLE(false);
    hall.pull=cfg->pullup?NRF_GPIO_PIN_PULLUP:NRF_GPIO_PIN_NOPULL;
    m_hall1_init_rc=NRF_SUCCESS; m_hall2_init_rc=NRF_SUCCESS;
    if(cfg->mode==NRFCLAW_HALL_MODE_SINGLE) {
        uint32_t pin=(cfg->channel==2U)?P_HALL2:P_HALL1;
        uint32_t rc=nrf_drv_gpiote_in_init(pin,&hall,gpiote_handler);
        if(cfg->channel==2U)m_hall2_init_rc=rc;else m_hall1_init_rc=rc;
        if(rc!=NRF_SUCCESS)return false;
        m_cfg=*cfg; m_pos=0; m_count=0; m_prev_ab=0;
        nrf_drv_gpiote_in_event_enable(pin,true);
        return true;
    }
    m_hall1_init_rc=nrf_drv_gpiote_in_init(P_HALL1,&hall,gpiote_handler);
    if(m_hall1_init_rc!=NRF_SUCCESS)return false;
    m_hall2_init_rc=nrf_drv_gpiote_in_init(P_HALL2,&hall,gpiote_handler);
    if(m_hall2_init_rc!=NRF_SUCCESS){nrf_drv_gpiote_in_uninit(P_HALL1);return false;}
    m_cfg=*cfg; m_cfg.channel=1U; m_pos=0; m_count=0;
    m_prev_ab=(uint8_t)((nrf_gpio_pin_read(P_HALL1)<<1)|nrf_gpio_pin_read(P_HALL2));
    nrf_drv_gpiote_in_event_enable(P_HALL1,true);
    nrf_drv_gpiote_in_event_enable(P_HALL2,true);
    return true;
}
bool nrfclaw_hall_enable(bool pullup) {
    nrfclaw_hall_config_t c={NRFCLAW_HALL_MODE_QUADRATURE,1U,pullup,true,true};
    return nrfclaw_hall_configure(&c);
}
bool nrfclaw_hall_active(void){return m_cfg.mode!=NRFCLAW_HALL_MODE_DISABLED;}
nrfclaw_hall_mode_t nrfclaw_hall_mode(void){return m_cfg.mode;}
int32_t nrfclaw_hall_position(void){return m_pos;}
uint32_t nrfclaw_hall_count(void){return m_count;}
uint8_t nrfclaw_hall_channel(void){return m_cfg.channel;}
void nrfclaw_hall_diag(uint8_t *h1,uint8_t *h2,uint32_t *r1,uint32_t *r2){if(h1)*h1=P_HALL1;if(h2)*h2=P_HALL2;if(r1)*r1=m_hall1_init_rc;if(r2)*r2=m_hall2_init_rc;}
