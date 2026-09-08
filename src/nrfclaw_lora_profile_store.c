#include "nrfclaw_lora_profile_store.h"
#include "nrf.h"
#include "nrf_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"
#include <string.h>

#define LORA_STORE_MAGIC   0x3152464CUL /* "LFR1" */
#define LORA_STORE_FORMAT  0x00010000UL
#define LORA_STORE_COMMIT  0x544D4F43UL /* "COMT" */
#define LORA_STORE_ERASED  0xFFFFFFFFUL

typedef struct {
    uint32_t magic;
    uint32_t format;
    uint32_t generation;
    uint32_t payload_len;
    uint32_t crc32;
    uint32_t commit;
    nrfclaw_lora_profile_t profile;
} lora_profile_image_t;

typedef enum {
    ST_IDLE=0,
    ST_ERASE_PENDING, ST_ERASE_WAIT,
    ST_BODY_PENDING, ST_BODY_WAIT,
    ST_COMMIT_PENDING, ST_COMMIT_WAIT,
    ST_VERIFY, ST_ERROR
} lora_store_state_t;

#define LORA_IMAGE_WORDS ((uint32_t)((sizeof(lora_profile_image_t)+3U)/4U))
#define LORA_COMMIT_WORD_INDEX 5U

static lora_store_state_t m_state;
static uint8_t m_active_slot;
static uint8_t m_target_slot;
static uint32_t m_generation;
static uint32_t m_target_addr;
static uint32_t m_stage[LORA_IMAGE_WORDS];
static uint32_t m_commit_word = LORA_STORE_COMMIT;
static volatile bool m_evt_ok, m_evt_err;
static bool m_have_pending;
static nrfclaw_lora_profile_t m_pending;

static uint32_t crc32_calc(uint8_t const *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i=0; i<len; ++i) {
        crc ^= data[i];
        for (uint8_t b=0; b<8U; ++b)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static uint32_t slot_addr(uint8_t slot)
{
    return slot ? NRFCLAW_LORA_PROFILE_SLOT_B_ADDR : NRFCLAW_LORA_PROFILE_SLOT_A_ADDR;
}

static bool gen_newer(uint32_t a, uint32_t b) { return (int32_t)(a-b) > 0; }

static bool geometry_ok(void)
{
    uint32_t total = NRF_FICR->CODESIZE * NRF_FICR->CODEPAGESIZE;
    return NRF_FICR->CODEPAGESIZE == 4096U &&
           total > NRFCLAW_LORA_PROFILE_SLOT_B_ADDR &&
           (NRFCLAW_LORA_PROFILE_SLOT_A_ADDR % 4096U) == 0U &&
           (NRFCLAW_LORA_PROFILE_SLOT_B_ADDR % 4096U) == 0U;
}

static bool slot_valid(uint8_t slot, nrfclaw_lora_profile_t *out, uint32_t *gen)
{
    lora_profile_image_t const *h =
        (lora_profile_image_t const *)(uintptr_t)slot_addr(slot);
    if (h->magic != LORA_STORE_MAGIC || h->format != LORA_STORE_FORMAT ||
        h->commit != LORA_STORE_COMMIT) return false;
    if (h->payload_len != sizeof(nrfclaw_lora_profile_t)) return false;
    if (crc32_calc((uint8_t const *)&h->profile, h->payload_len) != h->crc32) return false;
    if (!nrfclaw_lora_profile_validate(&h->profile)) return false;
    if (out) *out = h->profile;
    if (gen) *gen = h->generation;
    return true;
}

static void soc_evt(uint32_t e, void *ctx)
{
    (void)ctx;
    if (m_state != ST_ERASE_WAIT && m_state != ST_BODY_WAIT && m_state != ST_COMMIT_WAIT)
        return;
    if (e == NRF_EVT_FLASH_OPERATION_SUCCESS) m_evt_ok = true;
    else if (e == NRF_EVT_FLASH_OPERATION_ERROR) m_evt_err = true;
}

#ifndef NRFCLAW_LORA_PROFILE_STORE_SOC_OBSERVER_PRIO
#define NRFCLAW_LORA_PROFILE_STORE_SOC_OBSERVER_PRIO 1
#endif
STATIC_ASSERT(NRFCLAW_LORA_PROFILE_STORE_SOC_OBSERVER_PRIO < NRF_SDH_SOC_OBSERVER_PRIO_LEVELS,
              "LoRa profile store SoC observer priority unavailable");
NRF_SDH_SOC_OBSERVER(m_lora_profile_store_observer,
                     NRFCLAW_LORA_PROFILE_STORE_SOC_OBSERVER_PRIO,
                     soc_evt, NULL);

void nrfclaw_lora_profile_store_init(void)
{
    m_state = ST_IDLE;
    m_active_slot = 0xFFU;
    m_target_slot = 0U;
    m_generation = 0U;
    m_target_addr = 0U;
    m_evt_ok = m_evt_err = false;
    m_have_pending = false;
    memset(m_stage, 0, sizeof(m_stage));
    memset(&m_pending, 0, sizeof(m_pending));
    if (!geometry_ok()) { m_state = ST_ERROR; return; }

    uint32_t ga=0U, gb=0U;
    bool va = slot_valid(0U, NULL, &ga);
    bool vb = slot_valid(1U, NULL, &gb);
    if (va && (!vb || gen_newer(ga, gb))) { m_active_slot=0U; m_generation=ga; }
    else if (vb) { m_active_slot=1U; m_generation=gb; }
}

bool nrfclaw_lora_profile_store_load(nrfclaw_lora_profile_t *out)
{
    if (!out || m_active_slot > 1U) return false;
    return slot_valid(m_active_slot, out, NULL);
}

bool nrfclaw_lora_profile_store_request_save(nrfclaw_lora_profile_t const *profile)
{
    if (!profile || !nrfclaw_lora_profile_validate(profile) || m_state == ST_ERROR) return false;
    m_pending = *profile;
    m_have_pending = true; /* coalesce rapid user changes */
    return true;
}

static void start_pending(void)
{
    if (!m_have_pending || m_state != ST_IDLE) return;
    memset(m_stage, 0xFF, sizeof(m_stage));
    lora_profile_image_t *h = (lora_profile_image_t *)m_stage;
    h->magic = LORA_STORE_MAGIC;
    h->format = LORA_STORE_FORMAT;
    h->generation = m_generation + 1U;
    h->payload_len = sizeof(nrfclaw_lora_profile_t);
    h->profile = m_pending;
    h->crc32 = crc32_calc((uint8_t const *)&h->profile, sizeof(h->profile));
    h->commit = LORA_STORE_ERASED;
    m_target_slot = (m_active_slot == 0U) ? 1U : 0U;
    if (m_active_slot > 1U) m_target_slot = 0U;
    m_target_addr = slot_addr(m_target_slot);
    m_have_pending = false;
    m_evt_ok = m_evt_err = false;
    m_state = ST_ERASE_PENDING;
}

void nrfclaw_lora_profile_store_process(void)
{
    if (m_state == ST_ERROR) return;
    if (m_evt_err) { m_evt_err=false; m_state=ST_ERROR; return; }
    if (m_evt_ok) {
        m_evt_ok=false;
        if (m_state == ST_ERASE_WAIT) m_state=ST_BODY_PENDING;
        else if (m_state == ST_BODY_WAIT) m_state=ST_COMMIT_PENDING;
        else if (m_state == ST_COMMIT_WAIT) m_state=ST_VERIFY;
    }
    if (m_state == ST_IDLE) { start_pending(); return; }
    if (m_state == ST_ERASE_PENDING) {
        uint32_t e=sd_flash_page_erase(m_target_addr/NRF_FICR->CODEPAGESIZE);
        if (e==NRF_SUCCESS) m_state=ST_ERASE_WAIT;
        else if (e!=NRF_ERROR_BUSY) m_state=ST_ERROR;
    } else if (m_state == ST_BODY_PENDING) {
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)m_target_addr,
                                  m_stage, LORA_IMAGE_WORDS);
        if (e==NRF_SUCCESS) m_state=ST_BODY_WAIT;
        else if (e!=NRF_ERROR_BUSY) m_state=ST_ERROR;
    } else if (m_state == ST_COMMIT_PENDING) {
        uint32_t e=sd_flash_write((uint32_t *)(uintptr_t)(m_target_addr+LORA_COMMIT_WORD_INDEX*4U),
                                  &m_commit_word, 1U);
        if (e==NRF_SUCCESS) m_state=ST_COMMIT_WAIT;
        else if (e!=NRF_ERROR_BUSY) m_state=ST_ERROR;
    } else if (m_state == ST_VERIFY) {
        uint32_t g=0U;
        if (!slot_valid(m_target_slot, NULL, &g)) { m_state=ST_ERROR; return; }
        m_active_slot=m_target_slot;
        m_generation=g;
        m_state=ST_IDLE;
        start_pending();
    }
}

bool nrfclaw_lora_profile_store_busy(void) { return m_state != ST_IDLE || m_have_pending; }
uint32_t nrfclaw_lora_profile_store_generation(void) { return m_generation; }
