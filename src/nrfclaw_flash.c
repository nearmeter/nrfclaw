#include "nrfclaw_flash.h"
#include "nrfclaw_vm.h"
#include "nrfclaw_auth.h"

#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include <string.h>

#define SLOT_MAGIC              0x364C434EUL /* "NCL6" LE */
#define SLOT_FORMAT             0x00060001UL
#define SLOT_COMMIT             0x544D4F43UL /* "COMT" LE */
#define SLOT_ERASED             0xFFFFFFFFUL

#define SLOT_AUTH_WORDS         (NRFCLAW_AUTH_TAG_SIZE / 4U)
#define SLOT_SCHED_WORDS        (NRFCLAW_SCHEDULE_WIRE_SIZE / 4U)
#define SLOT_HEADER_WORDS       (6U + SLOT_AUTH_WORDS + SLOT_SCHED_WORDS)
#define SLOT_COMMIT_WORD_INDEX  (5U + SLOT_AUTH_WORDS + SLOT_SCHED_WORDS)
#define SLOT_PAYLOAD_OFFSET     (SLOT_HEADER_WORDS * 4U)
#define SLOT_IMAGE_WORDS        (SLOT_HEADER_WORDS + ((NRFCLAW_VM_MAX_PROGRAM_SIZE + 3U) / 4U))

typedef struct
{
    uint32_t magic;
    uint32_t format;
    uint32_t generation;
    uint32_t len_crc;      /* low16=len, high16=crc16 */
    uint32_t reserved;
    uint32_t auth[NRFCLAW_AUTH_TAG_SIZE / 4U];
    uint32_t schedule[NRFCLAW_SCHEDULE_WIRE_SIZE / 4U];
    uint32_t commit;       /* written LAST */
} slot_header_t;

typedef enum
{
    WR_IDLE = 0,
    WR_ERASE_PENDING,
    WR_ERASE_WAIT,
    WR_BODY_PENDING,
    WR_BODY_WAIT,
    WR_COMMIT_PENDING,
    WR_COMMIT_WAIT,
    WR_VERIFY_PENDING,
    WR_ERROR
} writer_state_t;

static writer_state_t m_wr_state;
static nrfclaw_flash_status_t m_status;

static uint8_t  m_active_slot;
static uint32_t m_generation;

static uint8_t  m_target_slot;
static uint32_t m_target_addr;

static uint32_t m_stage[SLOT_IMAGE_WORDS];
static uint32_t m_commit_word = SLOT_COMMIT;
static uint16_t m_stage_len;
static uint16_t m_stage_crc;
static uint8_t  m_stage_auth[NRFCLAW_AUTH_TAG_SIZE];
static nrfclaw_schedule_t m_stage_schedule;

static volatile bool m_flash_evt_success;
static volatile bool m_flash_evt_error;

static uint16_t crc16_ccitt(uint8_t const *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;

    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;

        for (uint8_t bit = 0; bit < 8U; bit++)
        {
            if (crc & 0x8000U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc <<= 1;
        }
    }

    return crc;
}

static uint32_t slot_addr(uint8_t slot)
{
    return slot == 0U ?
           NRFCLAW_FLASH_SLOT0_ADDR :
           NRFCLAW_FLASH_SLOT1_ADDR;
}

static bool geometry_ok(void)
{
    if (NRF_FICR->CODEPAGESIZE != 4096U)
        return false;

    if (NRF_FICR->CODESIZE * NRF_FICR->CODEPAGESIZE <=
        NRFCLAW_FLASH_SLOT1_ADDR)
        return false;

    if ((NRFCLAW_FLASH_SLOT0_ADDR % NRF_FICR->CODEPAGESIZE) != 0U ||
        (NRFCLAW_FLASH_SLOT1_ADDR % NRF_FICR->CODEPAGESIZE) != 0U)
        return false;

    return true;
}

static bool slot_valid(uint8_t slot,
                       uint16_t *len_out,
                       uint32_t *generation_out,
                       nrfclaw_schedule_t *schedule_out)
{
    uint32_t addr = slot_addr(slot);
    slot_header_t const *h =
        (slot_header_t const *)(uintptr_t)addr;

    if (h->magic != SLOT_MAGIC ||
        h->format != SLOT_FORMAT ||
        h->commit != SLOT_COMMIT)
        return false;

    uint16_t len = (uint16_t)(h->len_crc & 0xFFFFU);
    uint16_t crc = (uint16_t)(h->len_crc >> 16);

    if (len == 0U || len > NRFCLAW_VM_MAX_PROGRAM_SIZE)
        return false;

    uint8_t const *payload =
        (uint8_t const *)(uintptr_t)(addr + SLOT_PAYLOAD_OFFSET);

    if (crc16_ccitt(payload, len) != crc)
        return false;

    nrfclaw_schedule_t schedule;
    nrfclaw_schedule_decode(&schedule, (uint8_t const *)h->schedule);

    if (!nrfclaw_auth_verify(
            payload,
            len,
            &schedule,
            (uint8_t const *)h->auth))
        return false;

    if (schedule_out) *schedule_out = schedule;
    if (len_out) *len_out = len;
    if (generation_out) *generation_out = h->generation;

    return true;
}

static bool generation_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static void soc_evt_handler(uint32_t sys_evt, void *p_context)
{
    (void)p_context;

    if (sys_evt == NRF_EVT_FLASH_OPERATION_SUCCESS)
        m_flash_evt_success = true;
    else if (sys_evt == NRF_EVT_FLASH_OPERATION_ERROR)
        m_flash_evt_error = true;
}

NRF_SDH_SOC_OBSERVER(m_nrfclaw_flash_observer,
                     1,
                     soc_evt_handler,
                     NULL);

void nrfclaw_flash_init(void)
{
    m_wr_state = WR_IDLE;
    m_status = NRFCLAW_FLASH_EMPTY;
    m_active_slot = 0xFFU;
    m_generation = 0U;

    m_flash_evt_success = false;
    m_flash_evt_error = false;

    if (!geometry_ok())
    {
        ((void)0);

        m_status = NRFCLAW_FLASH_ERROR;
        return;
    }

    uint16_t len0 = 0, len1 = 0;
    uint32_t gen0 = 0, gen1 = 0;

    bool valid0 = slot_valid(0, &len0, &gen0, NULL);
    bool valid1 = slot_valid(1, &len1, &gen1, NULL);

    if (!valid0 && !valid1)
    {
        ((void)0);

        return;
    }

    if (valid0 && (!valid1 || generation_newer(gen0, gen1)))
    {
        m_active_slot = 0U;
        m_generation = gen0;
    }
    else
    {
        m_active_slot = 1U;
        m_generation = gen1;
    }

    m_status = NRFCLAW_FLASH_READY;

    ((void)0);
}

bool nrfclaw_flash_get_latest(uint8_t const **program,
                              uint16_t *len,
                              nrfclaw_schedule_t *schedule)
{
    if (!program || !len || !schedule ||
        m_active_slot > 1U ||
        m_status == NRFCLAW_FLASH_ERROR)
        return false;

    uint16_t recovered_len = 0;

    if (!slot_valid(m_active_slot,
                    &recovered_len,
                    NULL,
                    schedule))
        return false;

    *program = (uint8_t const *)(uintptr_t)
        (slot_addr(m_active_slot) + SLOT_PAYLOAD_OFFSET);

    *len = recovered_len;
    return true;
}

bool nrfclaw_flash_request_save(uint8_t const *program,
                                uint16_t len,
                                uint16_t crc16,
                                nrfclaw_schedule_t const *schedule,
                                uint8_t const auth_tag[NRFCLAW_AUTH_TAG_SIZE])
{
    if (!program ||
        !schedule ||
        !auth_tag ||
        len == 0U ||
        len > NRFCLAW_VM_MAX_PROGRAM_SIZE ||
        m_wr_state != WR_IDLE ||
        m_status == NRFCLAW_FLASH_ERROR)
        return false;

    /*
     * Always write the inactive slot.
     * If there is no active slot yet, start with slot 0.
     */
    m_target_slot =
        (m_active_slot == 0U) ? 1U : 0U;

    if (m_active_slot > 1U)
        m_target_slot = 0U;

    m_target_addr = slot_addr(m_target_slot);

    memset(m_stage, 0xFF, sizeof(m_stage));

    slot_header_t *h =
        (slot_header_t *)m_stage;

    h->magic = SLOT_MAGIC;
    h->format = SLOT_FORMAT;
    h->generation = m_generation + 1U;
    h->len_crc =
        ((uint32_t)crc16 << 16) |
        (uint32_t)len;
    h->reserved = SLOT_ERASED;
    memcpy(h->auth, auth_tag, NRFCLAW_AUTH_TAG_SIZE);
    nrfclaw_schedule_encode(schedule, (uint8_t *)h->schedule);

    /*
     * Critical atomicity rule:
     * commit remains erased in the body image and is written separately,
     * only after header+payload were written and verified.
     */
    h->commit = SLOT_ERASED;

    memcpy((uint8_t *)m_stage + SLOT_PAYLOAD_OFFSET,
           program,
           len);

    m_stage_len = len;
    m_stage_crc = crc16;
    memcpy(m_stage_auth, auth_tag, NRFCLAW_AUTH_TAG_SIZE);
    m_stage_schedule = *schedule;

    m_flash_evt_success = false;
    m_flash_evt_error = false;

    m_wr_state = WR_ERASE_PENDING;
    m_status = NRFCLAW_FLASH_SAVING;

    ((void)0);

    return true;
}

static void flash_fail(char const *msg)
{
    ((void)0);
    ((void)0);
    ((void)0);

    /*
     * The previous committed slot remains untouched and remains recoverable.
     */
    m_wr_state = WR_ERROR;
    m_status = NRFCLAW_FLASH_ERROR;
}

void nrfclaw_flash_process(void)
{
    if (m_wr_state == WR_IDLE ||
        m_wr_state == WR_ERROR)
        return;

    if (m_flash_evt_error)
    {
        m_flash_evt_error = false;
        flash_fail("SoftDevice flash operation");
        return;
    }

    if (m_flash_evt_success)
    {
        m_flash_evt_success = false;

        switch (m_wr_state)
        {
            case WR_ERASE_WAIT:
                m_wr_state = WR_BODY_PENDING;
                break;

            case WR_BODY_WAIT:
                m_wr_state = WR_VERIFY_PENDING;
                break;

            case WR_COMMIT_WAIT:
                m_wr_state = WR_VERIFY_PENDING;
                break;

            default:
                break;
        }
    }

    switch (m_wr_state)
    {
        case WR_ERASE_PENDING:
        {
            uint32_t page =
                m_target_addr / NRF_FICR->CODEPAGESIZE;

            uint32_t err =
                sd_flash_page_erase(page);

            if (err == NRF_SUCCESS)
                m_wr_state = WR_ERASE_WAIT;
            else if (err != NRF_ERROR_BUSY)
                flash_fail("erase rejected");

            break;
        }

        case WR_BODY_PENDING:
        {
            uint32_t words =
                SLOT_HEADER_WORDS +
                ((m_stage_len + 3U) / 4U);

            uint32_t err =
                sd_flash_write(
                    (uint32_t *)(uintptr_t)m_target_addr,
                    m_stage,
                    words);

            if (err == NRF_SUCCESS)
                m_wr_state = WR_BODY_WAIT;
            else if (err != NRF_ERROR_BUSY)
                flash_fail("body write rejected");

            break;
        }

        case WR_VERIFY_PENDING:
        {
            slot_header_t const *h =
                (slot_header_t const *)(uintptr_t)m_target_addr;

            uint8_t const *payload =
                (uint8_t const *)(uintptr_t)
                (m_target_addr + SLOT_PAYLOAD_OFFSET);

            if (h->commit == SLOT_ERASED)
            {
                nrfclaw_schedule_t stored_schedule;
                nrfclaw_schedule_decode(
                    &stored_schedule,
                    (uint8_t const *)h->schedule
                );

                if (h->magic != SLOT_MAGIC ||
                    h->format != SLOT_FORMAT ||
                    (uint16_t)(h->len_crc & 0xFFFFU) != m_stage_len ||
                    (uint16_t)(h->len_crc >> 16) != m_stage_crc ||
                    memcmp(h->auth, m_stage_auth, NRFCLAW_AUTH_TAG_SIZE) != 0 ||
                    memcmp(&stored_schedule, &m_stage_schedule,
                           sizeof(stored_schedule)) != 0 ||
                    crc16_ccitt(payload, m_stage_len) != m_stage_crc ||
                    !nrfclaw_auth_verify(
                        payload,
                        m_stage_len,
                        &stored_schedule,
                        (uint8_t const *)h->auth))
                {
                    flash_fail("body verify failed");
                    break;
                }

                m_wr_state = WR_COMMIT_PENDING;
            }
            else
            {
                if (!slot_valid(m_target_slot, NULL, NULL, NULL))
                {
                    flash_fail("committed slot verify failed");
                    break;
                }

                m_active_slot = m_target_slot;
                m_generation++;

                m_wr_state = WR_IDLE;
                m_status = NRFCLAW_FLASH_READY;

                ((void)0);
            }

            break;
        }

        case WR_COMMIT_PENDING:
        {
            uint32_t *dst =
                (uint32_t *)(uintptr_t)
                (m_target_addr +
                 SLOT_COMMIT_WORD_INDEX * 4U);

            uint32_t err =
                sd_flash_write(
                    dst,
                    &m_commit_word,
                    1U);

            if (err == NRF_SUCCESS)
                m_wr_state = WR_COMMIT_WAIT;
            else if (err != NRF_ERROR_BUSY)
                flash_fail("commit write rejected");

            break;
        }

        default:
            break;
    }
}

nrfclaw_flash_status_t nrfclaw_flash_status(void)
{
    return m_status;
}

uint8_t nrfclaw_flash_active_slot(void)
{
    return m_active_slot;
}

uint32_t nrfclaw_flash_generation(void)
{
    return m_generation;
}
