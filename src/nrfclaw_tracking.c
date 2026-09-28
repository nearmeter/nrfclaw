#include "nrfclaw_tracking.h"

#include "nrfclaw_ble.h"
#include "nrfclaw_ble_app.h"
#include "nrfclaw_sha256.h"
#include "nrfclaw_rtc.h"

#include "app_timer.h"
#include "app_util.h"
#include "ble_gap.h"
#include "nrf.h"
#include "nrf_error.h"
#include "nrf_sdh_soc.h"
#include "nrf_soc.h"
#include "uECC.h"
#include <stddef.h>
#include <string.h>

#define TRACKING_CONN_CFG_TAG   1U
#define TRACKING_STORE_MAGIC    0x324B5254UL /* "TRK2" */
#define TRACKING_STORE_VERSION  2U

/* Stage 8.3 monotonic rotation journal. */
#define TRACKING_JOURNAL_A_ADDR 0x00076000UL
#define TRACKING_JOURNAL_B_ADDR 0x00077000UL
#define TRACKING_ROTATION_TIMER_CHUNK_S 300UL

typedef struct
{
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    uint8_t  seed[NRFCLAW_TRACKING_SEED_SIZE];
    uint32_t rotation_seconds;
    uint16_t adv_interval_ms;
    int8_t   tx_power_dbm;
    uint8_t  flags;
    uint32_t crc32;
} tracking_store_t;

typedef enum
{
    COMMIT_IDLE = 0,
    COMMIT_ERASE_PENDING,
    COMMIT_ERASE_WAIT,
    COMMIT_WRITE_PENDING,
    COMMIT_WRITE_WAIT,
    COMMIT_ERROR
} commit_state_t;

typedef struct
{
    uint32_t index;
    uint32_t index_inv;
    uint32_t epoch;
    uint32_t epoch_inv;
} tracking_journal_record_t;

typedef enum
{
    JOURNAL_IDLE = 0,
    JOURNAL_ERASE_PENDING,
    JOURNAL_ERASE_WAIT,
    JOURNAL_WRITE_PENDING,
    JOURNAL_WRITE_WAIT,
    JOURNAL_ERROR
} journal_state_t;

APP_TIMER_DEF(m_rotation_timer);
APP_TIMER_DEF(m_apply_retry_timer);

static uint8_t m_adv_data[31];
static uint8_t m_adv_key[NRFCLAW_TRACKING_KEY_SIZE];
static uint8_t m_manual_key[NRFCLAW_TRACKING_KEY_SIZE];

static bool m_manual_key_valid;
static bool m_active;
static bool m_rotate_pending;

/*
 * Stage 8.3 guarantees that a derived key is never advertised before its
 * monotonic index has been committed to Flash.
 */
static bool m_start_pending;
static bool m_index_commit_pending;
static bool m_apply_pending;
static uint32_t m_pending_index;

/* app_timer is kept below its RTC counter range by using <=300 s chunks. */
static uint32_t m_rotation_remaining_s;

static uint32_t m_key_index;
static uint32_t m_rotation_seconds = NRFCLAW_TRACKING_DEFAULT_ROTATION_S;
static uint16_t m_interval_ms = NRFCLAW_TRACKING_DEFAULT_INTERVAL_MS;
static int8_t m_tx_power_dbm = NRFCLAW_TRACKING_DEFAULT_TX_DBM;

static ble_gap_addr_t m_normal_addr;
static bool m_normal_addr_valid;

static tracking_store_t m_stage;
static tracking_store_t const *m_store;
static uint8_t m_active_slot = 0xFFU;
static commit_state_t m_commit_state;
static uint32_t m_commit_addr;
static volatile bool m_flash_ok;
static volatile bool m_flash_error;

static journal_state_t m_journal_state;
static tracking_journal_record_t m_journal_pending;
static uint32_t m_journal_pending_addr;
static uint32_t m_journal_active_page;
static uint32_t m_journal_next_addr;
static uint32_t m_journal_index;
static uint32_t m_journal_epoch;
static bool m_journal_valid;
static volatile bool m_journal_flash_ok;
static volatile bool m_journal_flash_error;

/*
 * Local CRC32/IEEE 802.3 implementation for the tiny tracking identity store.
 *
 * Polynomial (reflected): 0xEDB88320
 * Init: 0xFFFFFFFF
 * Final XOR: 0xFFFFFFFF
 *
 * Keeping this local avoids depending on the optional nRF5 SDK crc32 module.
 */
static uint32_t tracking_crc32(
    uint8_t const * data,
    uint32_t len
)
{
    uint32_t crc =
        0xFFFFFFFFUL;

    while (len--)
    {
        crc ^=
            *data++;

        for (uint8_t bit = 0U;
             bit < 8U;
             bit++)
        {
            uint32_t mask =
                (uint32_t)(
                    -(int32_t)(
                        crc & 1U
                    )
                );

            crc =
                (crc >> 1U) ^
                (
                    0xEDB88320UL &
                    mask
                );
        }
    }

    return
        crc ^ 0xFFFFFFFFUL;
}


static uint32_t store_crc(
    tracking_store_t const * s
)
{
    return tracking_crc32(
        (uint8_t const *)s,
        (uint32_t)offsetof(
            tracking_store_t,
            crc32
        )
    );
}

static bool store_valid(tracking_store_t const *s)
{
    if (s->magic != TRACKING_STORE_MAGIC ||
        s->version != TRACKING_STORE_VERSION)
        return false;

    if (s->rotation_seconds == 0U ||
        s->adv_interval_ms < 100U ||
        s->adv_interval_ms > 10000U)
        return false;

    return store_crc(s) == s->crc32;
}

static bool generation_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static void load_store(void)
{
    tracking_store_t const *a =
        (tracking_store_t const *)NRFCLAW_TRACKING_SLOT_A_ADDR;
    tracking_store_t const *b =
        (tracking_store_t const *)NRFCLAW_TRACKING_SLOT_B_ADDR;

    bool va = store_valid(a);
    bool vb = store_valid(b);

    m_store = NULL;
    m_active_slot = 0xFFU;

    if (va && vb)
    {
        if (generation_newer(a->generation, b->generation))
        {
            m_store = a;
            m_active_slot = 0U;
        }
        else
        {
            m_store = b;
            m_active_slot = 1U;
        }
    }
    else if (va)
    {
        m_store = a;
        m_active_slot = 0U;
    }
    else if (vb)
    {
        m_store = b;
        m_active_slot = 1U;
    }

    if (m_store)
    {
        m_rotation_seconds = m_store->rotation_seconds;
        m_interval_ms = m_store->adv_interval_ms;
        m_tx_power_dbm = m_store->tx_power_dbm;
    }
}


static bool journal_record_erased(
    tracking_journal_record_t const * r
)
{
    return
        r->index == 0xFFFFFFFFUL &&
        r->index_inv == 0xFFFFFFFFUL &&
        r->epoch == 0xFFFFFFFFUL &&
        r->epoch_inv == 0xFFFFFFFFUL;
}


static bool journal_record_valid(
    tracking_journal_record_t const * r
)
{
    return
        !journal_record_erased(r) &&
        r->index_inv == ~r->index &&
        r->epoch_inv == ~r->epoch;
}


static void journal_scan_page(
    uint32_t page_addr,
    bool * valid,
    uint32_t * best_index,
    uint32_t * best_epoch,
    uint32_t * first_erased
)
{
    *valid = false;
    *best_index = 0U;
    *best_epoch = 0U;
    *first_erased = 0U;

    uint32_t const count =
        NRF_FICR->CODEPAGESIZE /
        sizeof(tracking_journal_record_t);

    tracking_journal_record_t const * records =
        (tracking_journal_record_t const *)page_addr;

    for (uint32_t i = 0U; i < count; i++)
    {
        tracking_journal_record_t const * r =
            &records[i];

        if (journal_record_erased(r))
        {
            if (*first_erased == 0U)
            {
                *first_erased =
                    page_addr +
                    i * sizeof(tracking_journal_record_t);
            }

            continue;
        }

        if (!journal_record_valid(r))
            continue;

        if (!*valid ||
            r->index > *best_index)
        {
            *valid = true;
            *best_index = r->index;
            *best_epoch = r->epoch;
        }
    }
}


static void journal_load(void)
{
    bool va = false;
    bool vb = false;
    uint32_t ia = 0U;
    uint32_t ib = 0U;
    uint32_t ta = 0U;
    uint32_t tb = 0U;
    uint32_t ea = 0U;
    uint32_t eb = 0U;

    journal_scan_page(
        TRACKING_JOURNAL_A_ADDR,
        &va,
        &ia,
        &ta,
        &ea
    );

    journal_scan_page(
        TRACKING_JOURNAL_B_ADDR,
        &vb,
        &ib,
        &tb,
        &eb
    );

    m_journal_valid = false;
    m_journal_index = 0U;
    m_journal_epoch = 0U;
    m_journal_active_page = TRACKING_JOURNAL_A_ADDR;
    m_journal_next_addr =
        ea ? ea : TRACKING_JOURNAL_A_ADDR;

    if (va && vb)
    {
        if (ib > ia)
        {
            m_journal_valid = true;
            m_journal_index = ib;
            m_journal_epoch = tb;
            m_journal_active_page =
                TRACKING_JOURNAL_B_ADDR;
            m_journal_next_addr = eb;
        }
        else
        {
            m_journal_valid = true;
            m_journal_index = ia;
            m_journal_epoch = ta;
            m_journal_active_page =
                TRACKING_JOURNAL_A_ADDR;
            m_journal_next_addr = ea;
        }
    }
    else if (va)
    {
        m_journal_valid = true;
        m_journal_index = ia;
        m_journal_epoch = ta;
        m_journal_active_page =
            TRACKING_JOURNAL_A_ADDR;
        m_journal_next_addr = ea;
    }
    else if (vb)
    {
        m_journal_valid = true;
        m_journal_index = ib;
        m_journal_epoch = tb;
        m_journal_active_page =
            TRACKING_JOURNAL_B_ADDR;
        m_journal_next_addr = eb;
    }
}


static uint32_t journal_epoch_now(void)
{
    return
        nrfclaw_rtc_is_valid()
        ? nrfclaw_rtc_now()
        : 0U;
}


static uint32_t journal_choose_next_index(void)
{
    if (!m_journal_valid)
        return 0U;

    /*
     * Always advance at least once after a restart. This deliberately favors
     * skipping a key over ever reusing a key that may already have been on air.
     */
    uint32_t next =
        m_journal_index + 1U;

    /*
     * If Unix epoch is available, catch up any complete rotation windows that
     * elapsed since the last durable journal record.
     */
    if (nrfclaw_rtc_is_valid() &&
        m_journal_epoch != 0U &&
        m_rotation_seconds != 0U)
    {
        uint32_t now =
            nrfclaw_rtc_now();

        if (now > m_journal_epoch)
        {
            uint32_t elapsed =
                now - m_journal_epoch;

            uint32_t slots =
                elapsed / m_rotation_seconds;

            if (slots > 0U)
            {
                uint32_t by_time =
                    m_journal_index + slots;

                if (by_time > next)
                    next = by_time;
            }
        }
    }

    return next;
}


static bool journal_begin_append(
    uint32_t index,
    uint32_t epoch
)
{
    if (m_journal_state != JOURNAL_IDLE)
        return false;

    if (m_journal_valid &&
        index <= m_journal_index)
    {
        return false;
    }

    m_journal_pending.index =
        index;

    m_journal_pending.index_inv =
        ~index;

    m_journal_pending.epoch =
        epoch;

    m_journal_pending.epoch_inv =
        ~epoch;

    if (m_journal_next_addr != 0U)
    {
        m_journal_pending_addr =
            m_journal_next_addr;

        m_journal_state =
            JOURNAL_WRITE_PENDING;

        return true;
    }

    /*
     * Current page is full. Keep it intact while erasing the other page.
     * A power failure during compaction therefore still leaves the old page
     * fully recoverable.
     */
    m_journal_pending_addr =
        (m_journal_active_page ==
         TRACKING_JOURNAL_A_ADDR)
        ? TRACKING_JOURNAL_B_ADDR
        : TRACKING_JOURNAL_A_ADDR;

    m_journal_state =
        JOURNAL_ERASE_PENDING;

    return true;
}


static bool journal_page_is_erased(uint32_t page_addr)
{
    uint32_t const *p = (uint32_t const *)page_addr;
    uint32_t words = NRF_FICR->CODEPAGESIZE / sizeof(uint32_t);

    for (uint32_t i = 0U; i < words; i++)
    {
        if (p[i] != 0xFFFFFFFFUL)
            return false;
    }

    return true;
}


static bool journal_pending_record_is_committed(void)
{
    tracking_journal_record_t const *r =
        (tracking_journal_record_t const *)m_journal_pending_addr;

    return journal_record_valid(r) &&
           r->index == m_journal_pending.index &&
           r->epoch == m_journal_pending.epoch;
}


static void journal_process(void)
{
    /*
     * Stage 8.3 Flash-readback fix.
     *
     * The journal does not rely on NRF_EVT_FLASH_OPERATION_SUCCESS to
     * determine ownership/completion of its sd_flash_* operation. Other
     * nRFClaw modules may also perform SoftDevice Flash operations.
     *
     * Flash is memory mapped on nRF52, so completion is confirmed by
     * reading back the exact page/record in the main loop.
     */

    if (m_journal_state == JOURNAL_ERASE_PENDING)
    {
        uint32_t page =
            m_journal_pending_addr / NRF_FICR->CODEPAGESIZE;

        uint32_t err = sd_flash_page_erase(page);

        if (err == NRF_SUCCESS)
            m_journal_state = JOURNAL_ERASE_WAIT;
        else if (err != NRF_ERROR_BUSY)
            m_journal_state = JOURNAL_ERROR;

        return;
    }

    if (m_journal_state == JOURNAL_ERASE_WAIT)
    {
        uint32_t page_addr =
            (m_journal_pending_addr / NRF_FICR->CODEPAGESIZE) *
            NRF_FICR->CODEPAGESIZE;

        if (journal_page_is_erased(page_addr))
            m_journal_state = JOURNAL_WRITE_PENDING;

        return;
    }

    if (m_journal_state == JOURNAL_WRITE_PENDING)
    {
        uint32_t err =
            sd_flash_write(
                (uint32_t *)m_journal_pending_addr,
                (uint32_t const *)&m_journal_pending,
                sizeof(m_journal_pending) / sizeof(uint32_t));

        /*
         * Expose the journal Flash API result through tracking-debug.
         * This field is later overwritten by GAP calls once radio startup
         * begins, which is exactly what we want diagnostically.
         */

        if (err == NRF_SUCCESS)
            m_journal_state = JOURNAL_WRITE_WAIT;
        else if (err != NRF_ERROR_BUSY)
            m_journal_state = JOURNAL_ERROR;

        return;
    }

    if (m_journal_state == JOURNAL_WRITE_WAIT)
    {
        if (journal_pending_record_is_committed())
        {
            m_journal_state = JOURNAL_IDLE;
            journal_load();
        }

        return;
    }
}


static bool begin_index_commit(void)
{
    if (m_index_commit_pending)
        return true;

    uint32_t next =
        journal_choose_next_index();

    if (!journal_begin_append(
            next,
            journal_epoch_now()))
    {
        return false;
    }

    m_pending_index =
        next;

    m_index_commit_pending =
        true;

    /*
     * IMPORTANT:
     *
     * Do not leave the journal only in WRITE_PENDING/ERASE_PENDING waiting
     * for a future main-loop iteration. After the VM reaches DONE the system
     * can immediately enter sd_app_evt_wait(), and no event exists yet to
     * wake it because the Flash operation has not actually been started.
     *
     * Start the pending journal transaction now. Once sd_flash_write() or
     * sd_flash_page_erase() is accepted by the SoftDevice, its completion
     * generates the event that naturally wakes the main loop.
     */
    journal_process();

    return true;
}


static bool index_commit_finished(void)
{
    if (!m_index_commit_pending)
        return false;

    if (m_journal_state ==
        JOURNAL_ERROR)
    {
        m_index_commit_pending = false;
        return false;
    }

    if (m_journal_state !=
        JOURNAL_IDLE)
    {
        return false;
    }

    if (!m_journal_valid ||
        m_journal_index !=
        m_pending_index)
    {
        return false;
    }

    m_index_commit_pending =
        false;

    m_key_index =
        m_journal_index;

    return true;
}


static bool random_fill(uint8_t *dst, uint32_t len)
{
    while (len)
    {
        uint8_t available = 0U;

        if (sd_rand_application_bytes_available_get(&available) != NRF_SUCCESS)
            return false;

        if (available == 0U)
            continue;

        uint8_t n = (available < len) ? available : (uint8_t)len;

        if (sd_rand_application_vector_get(dst, n) != NRF_SUCCESS)
            return false;

        dst += n;
        len -= n;
    }

    return true;
}

static bool identity_create(void)
{
    memset(&m_stage, 0, sizeof(m_stage));

    m_stage.magic = TRACKING_STORE_MAGIC;
    m_stage.version = TRACKING_STORE_VERSION;
    m_stage.generation = m_store ? (m_store->generation + 1U) : 1U;
    m_stage.rotation_seconds = m_rotation_seconds;
    m_stage.adv_interval_ms = m_interval_ms;
    m_stage.tx_power_dbm = m_tx_power_dbm;

    if (!random_fill(m_stage.seed, sizeof(m_stage.seed)))
        return false;

    m_stage.crc32 = store_crc(&m_stage);

    m_commit_addr =
        (m_active_slot == 0U)
        ? NRFCLAW_TRACKING_SLOT_B_ADDR
        : NRFCLAW_TRACKING_SLOT_A_ADDR;

    m_commit_state = COMMIT_ERASE_PENDING;

    /*
     * Use staging immediately so first enable does not require a second boot.
     * The asynchronous commit follows in nrfclaw_tracking_process().
     */
    m_store = &m_stage;

    return true;
}

/*
 * Derive a deterministic P-224 private key:
 *
 * SHA256("nRFClaw-OH1" || seed || index || retry)
 *
 * The first 28 bytes are tested against secp224r1's valid private-key range.
 * The corresponding advertised key is the 28-byte X coordinate of the
 * public point, matching OpenHaystack's advertised-key representation.
 */
static bool derive_adv_key(uint32_t index,
                           uint8_t out_key[NRFCLAW_TRACKING_KEY_SIZE])
{
    static uint8_t const label[] = {
        'n','R','F','C','l','a','w','-','O','H','1'
    };

    uint8_t material[
        sizeof(label) +
        NRFCLAW_TRACKING_SEED_SIZE +
        4U +
        1U
    ];

    uint8_t digest[32];
    uint8_t private_key[28];
    uint8_t public_key[56];

    uECC_Curve curve = uECC_secp224r1();

    if (!m_store || !curve)
        return false;

    uint32_t p = 0U;

    memcpy(&material[p], label, sizeof(label));
    p += sizeof(label);

    memcpy(&material[p], m_store->seed, NRFCLAW_TRACKING_SEED_SIZE);
    p += NRFCLAW_TRACKING_SEED_SIZE;

    material[p++] = (uint8_t)(index >> 24);
    material[p++] = (uint8_t)(index >> 16);
    material[p++] = (uint8_t)(index >> 8);
    material[p++] = (uint8_t)index;

    for (uint16_t retry = 0U; retry < 256U; retry++)
    {
        material[p] = (uint8_t)retry;

        nrfclaw_sha256(
            material,
            sizeof(material),
            digest
        );

        memcpy(private_key, digest, sizeof(private_key));

        /*
         * Current micro-ecc exposes uECC_compute_public_key(), but not
         * uECC_valid_private_key(). compute_public_key() returns 0 when the
         * candidate private key is invalid/out of range, so it is sufficient
         * for our deterministic retry loop.
         */
        if (!uECC_compute_public_key(private_key, public_key, curve))
            continue;

        /*
         * Stage 8.4 interoperability:
         *
         * micro-ecc is built with uECC_VLI_NATIVE_LITTLE_ENDIAN=1 for the
         * nRF52832, therefore public_key[0..27] is X in little-endian native
         * form. OpenHaystack / FindMy use the standard P-224 representation:
         * X is 28 bytes, big-endian.
         *
         * Keep micro-ecc native-little-endian internally for its lower stack
         * usage, but serialize the advertised key in standard big-endian.
         */
        for (uint32_t i = 0U;
             i < NRFCLAW_TRACKING_KEY_SIZE;
             i++)
        {
            out_key[i] =
                public_key[
                    NRFCLAW_TRACKING_KEY_SIZE -
                    1U -
                    i
                ];
        }

        memset(private_key, 0, sizeof(private_key));
        memset(public_key, 0, sizeof(public_key));
        memset(digest, 0, sizeof(digest));

        return true;
    }

    memset(private_key, 0, sizeof(private_key));
    memset(public_key, 0, sizeof(public_key));
    memset(digest, 0, sizeof(digest));

    return false;
}

static void build_payload(void)
{
    memset(m_adv_data, 0, sizeof(m_adv_data));

    m_adv_data[0] = 0x1EU;
    m_adv_data[1] = 0xFFU;
    m_adv_data[2] = 0x4CU;
    m_adv_data[3] = 0x00U;
    m_adv_data[4] = 0x12U;
    m_adv_data[5] = 0x19U;
    m_adv_data[6] = 0x00U;

    memcpy(&m_adv_data[7], &m_adv_key[6], 22U);
    m_adv_data[29] = (uint8_t)(m_adv_key[0] >> 6);
    m_adv_data[30] = 0x00U;
}

static void build_tracking_address(ble_gap_addr_t *addr)
{
    memset(addr, 0, sizeof(*addr));

    addr->addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;

    addr->addr[5] = (uint8_t)(m_adv_key[0] | 0xC0U);
    addr->addr[4] = m_adv_key[1];
    addr->addr[3] = m_adv_key[2];
    addr->addr[2] = m_adv_key[3];
    addr->addr[1] = m_adv_key[4];
    addr->addr[0] = m_adv_key[5];
}

static nrfclaw_tracking_status_t apply_advertising(void)
{
    uint8_t adv_handle =
        nrfclaw_ble_shared_adv_handle();

    if (adv_handle == BLE_GAP_ADV_SET_HANDLE_NOT_SET)
    {
        return NRFCLAW_TRACKING_ERROR;
    }

    uint32_t err =
        sd_ble_gap_adv_stop(adv_handle);

    if (err != NRF_SUCCESS &&
        err != NRF_ERROR_INVALID_STATE)
        return NRFCLAW_TRACKING_BUSY;


    ble_gap_addr_t addr;
    build_tracking_address(&addr);

    err = sd_ble_gap_addr_set(&addr);

    if (err == NRF_ERROR_INVALID_STATE)
        return NRFCLAW_TRACKING_BUSY;

    if (err != NRF_SUCCESS)
        return NRFCLAW_TRACKING_ERROR;


    build_payload();

    ble_gap_adv_data_t data;
    memset(&data, 0, sizeof(data));

    data.adv_data.p_data = m_adv_data;
    data.adv_data.len = sizeof(m_adv_data);

    ble_gap_adv_params_t params;
    memset(&params, 0, sizeof(params));

    params.properties.type =
        BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;

    params.interval =
        MSEC_TO_UNITS(m_interval_ms, UNIT_0_625_MS);

    params.duration = 0U;
    params.primary_phy = BLE_GAP_PHY_1MBPS;
    params.filter_policy = BLE_GAP_ADV_FP_ANY;

    err =
        sd_ble_gap_adv_set_configure(
            &adv_handle,
            &data,
            &params
        );

    if (err != NRF_SUCCESS)
        return NRFCLAW_TRACKING_ERROR;

    err =
        sd_ble_gap_tx_power_set(
            BLE_GAP_TX_POWER_ROLE_ADV,
            adv_handle,
            m_tx_power_dbm
        );

    if (err != NRF_SUCCESS)
        return NRFCLAW_TRACKING_BAD_ARG;

    err =
        sd_ble_gap_adv_start(
            adv_handle,
            TRACKING_CONN_CFG_TAG
        );

    if (err != NRF_SUCCESS)
        return NRFCLAW_TRACKING_ERROR;

    return NRFCLAW_TRACKING_OK;
}

static void arm_rotation_timer_chunk(void)
{
    if (!m_active ||
        m_rotation_remaining_s == 0U)
    {
        return;
    }

    uint32_t chunk_s =
        (m_rotation_remaining_s >
         TRACKING_ROTATION_TIMER_CHUNK_S)
        ? TRACKING_ROTATION_TIMER_CHUNK_S
        : m_rotation_remaining_s;

    m_rotation_remaining_s -=
        chunk_s;

    (void)app_timer_start(
        m_rotation_timer,
        APP_TIMER_TICKS(
            chunk_s * 1000UL
        ),
        NULL
    );
}


static void apply_retry_timer_handler(void *context)
{
    /*
     * Intentionally empty.
     *
     * Expiration itself wakes the CPU and gives the main loop another chance
     * to execute nrfclaw_tracking_process() with m_apply_pending still set.
     */
    (void)context;
}


static void rotation_timer_handler(void *context)
{
    (void)context;

    if (!m_active)
        return;

    if (m_rotation_remaining_s != 0U)
    {
        arm_rotation_timer_chunk();
        return;
    }

    m_rotate_pending =
        true;
}


static void arm_rotation_timer(void)
{
    (void)app_timer_stop(
        m_rotation_timer
    );

    if (!m_active)
        return;

    m_rotation_remaining_s =
        m_rotation_seconds;

    arm_rotation_timer_chunk();
}


static void tracking_soc_evt(uint32_t evt, void *ctx)
{
    (void)ctx;

    if (m_commit_state == COMMIT_ERASE_WAIT ||
        m_commit_state == COMMIT_WRITE_WAIT)
    {
        if (evt ==
            NRF_EVT_FLASH_OPERATION_SUCCESS)
        {
            m_flash_ok =
                true;
        }
        else if (evt ==
                 NRF_EVT_FLASH_OPERATION_ERROR)
        {
            m_flash_error =
                true;
        }

        return;
    }

}

NRF_SDH_SOC_OBSERVER(m_tracking_soc_observer, 0, tracking_soc_evt, NULL);

void nrfclaw_tracking_init(void)
{
    memset(m_adv_key, 0, sizeof(m_adv_key));
    memset(m_manual_key, 0, sizeof(m_manual_key));
    memset(&m_stage, 0, sizeof(m_stage));
    memset(&m_normal_addr, 0, sizeof(m_normal_addr));

    m_manual_key_valid = false;
    m_active = false;
    m_rotate_pending = false;
    m_start_pending = false;
    m_index_commit_pending = false;
    m_apply_pending = false;
    m_pending_index = 0U;
    m_rotation_remaining_s = 0U;
    m_key_index = 0U;

    m_commit_state = COMMIT_IDLE;
    m_flash_ok = false;
    m_flash_error = false;

    m_journal_state = JOURNAL_IDLE;
    m_journal_flash_ok = false;
    m_journal_flash_error = false;
    m_journal_valid = false;
    m_journal_index = 0U;
    m_journal_epoch = 0U;
    m_journal_active_page =
        TRACKING_JOURNAL_A_ADDR;
    m_journal_next_addr =
        TRACKING_JOURNAL_A_ADDR;

    m_rotation_seconds = NRFCLAW_TRACKING_DEFAULT_ROTATION_S;
    m_interval_ms = NRFCLAW_TRACKING_DEFAULT_INTERVAL_MS;
    m_tx_power_dbm = NRFCLAW_TRACKING_DEFAULT_TX_DBM;

    if (sd_ble_gap_addr_get(&m_normal_addr) == NRF_SUCCESS)
        m_normal_addr_valid = true;
    else
        m_normal_addr_valid = false;

    load_store();
    journal_load();

    /*
     * Expose the last durable index immediately in tracking-info even while
     * TRACKING is inactive after P0.21.
     */
    if (m_journal_valid)
    {
        m_key_index =
            m_journal_index;
    }

    (void)app_timer_create(
        &m_rotation_timer,
        APP_TIMER_MODE_SINGLE_SHOT,
        rotation_timer_handler
    );

    (void)app_timer_create(
        &m_apply_retry_timer,
        APP_TIMER_MODE_SINGLE_SHOT,
        apply_retry_timer_handler
    );
}


void nrfclaw_tracking_process(void)
{
    /*
     * Identity/config A/B store.
     */
    if (m_flash_error)
    {
        m_flash_error = false;
        m_commit_state = COMMIT_ERROR;
    }

    if (m_flash_ok)
    {
        m_flash_ok = false;

        if (m_commit_state ==
            COMMIT_ERASE_WAIT)
        {
            m_commit_state =
                COMMIT_WRITE_PENDING;
        }
        else if (m_commit_state ==
                 COMMIT_WRITE_WAIT)
        {
            m_commit_state =
                COMMIT_IDLE;

            load_store();
        }
    }

    if (m_commit_state ==
        COMMIT_ERASE_PENDING)
    {
        uint32_t page =
            m_commit_addr /
            NRF_FICR->CODEPAGESIZE;

        uint32_t err =
            sd_flash_page_erase(
                page
            );

        if (err == NRF_SUCCESS)
        {
            m_commit_state =
                COMMIT_ERASE_WAIT;
        }
        else if (err != NRF_ERROR_BUSY)
        {
            m_commit_state =
                COMMIT_ERROR;
        }
    }
    else if (m_commit_state ==
             COMMIT_WRITE_PENDING)
    {
        uint32_t words =
            (sizeof(m_stage) + 3U) /
            4U;

        uint32_t err =
            sd_flash_write(
                (uint32_t *)m_commit_addr,
                (uint32_t const *)&m_stage,
                words
            );

        if (err == NRF_SUCCESS)
        {
            m_commit_state =
                COMMIT_WRITE_WAIT;
        }
        else if (err != NRF_ERROR_BUSY)
        {
            m_commit_state =
                COMMIT_ERROR;
        }
    }

    /*
     * Stage 8.3 monotonic journal.
     *
     * Identity/config Flash and journal Flash are deliberately serialized.
     */
    if (m_commit_state ==
        COMMIT_IDLE)
    {
        journal_process();
    }


    /*
     * Initial autonomous start:
     *
     * The seed/config must be durable first. Then journal the first/next
     * index. Only after that record is confirmed do we derive and advertise.
     */
    if (m_start_pending)
    {
        if (m_commit_state !=
            COMMIT_IDLE)
        {
            return;
        }

        if (!m_store)
            return;

        if (!m_index_commit_pending)
        {
            if (!begin_index_commit())
            {
                return;
            }
        }

        if (!m_apply_pending)
        {
            if (!index_commit_finished())
            {
                return;
            }

            if (!derive_adv_key(
                    m_key_index,
                    m_adv_key))
            {
                return;
            }

            m_apply_pending = true;

            /*
             * Continue in THIS SAME main-loop pass.
             *
             * Returning here would allow the CPU to enter sd_app_evt_wait()
             * before any new asynchronous operation had been started, leaving
             * no event capable of waking it to perform GAP configuration.
             */
        }

        if (apply_advertising() !=
            NRFCLAW_TRACKING_OK)
        {
            /*
             * Retry this SAME durable index; do not allocate another one.
             */
            /*
             * GAP may transiently report BUSY/INVALID_STATE immediately after
             * Flash activity. Ensure there is a future RTC event to wake the
             * CPU and retry this SAME committed index.
             */
            (void)app_timer_start(
                m_apply_retry_timer,
                APP_TIMER_TICKS(20U),
                NULL
            );

            return;
        }

        (void)app_timer_stop(m_apply_retry_timer);
        m_apply_pending = false;

        m_active =
            true;

        ((void)0);

        m_start_pending =
            false;

        arm_rotation_timer();

        return;
    }


    /*
     * Rotation:
     *
     * Keep the old key on air while N+1 is being journaled. Once N+1 is
     * durable, switch address/payload atomically at the BLE level.
     */
    if (m_active &&
        m_rotate_pending)
    {
        if (!m_index_commit_pending)
        {
            if (!begin_index_commit())
            {
                return;
            }
        }

        if (!m_apply_pending)
        {
            if (!index_commit_finished())
            {
                return;
            }

            if (!derive_adv_key(
                    m_key_index,
                    m_adv_key))
            {
                return;
            }

            m_apply_pending = true;

            /*
             * Apply the new key immediately. Do not sleep waiting for an
             * event that has not been scheduled.
             */
        }

        if (apply_advertising() !=
            NRFCLAW_TRACKING_OK)
        {
            (void)app_timer_start(
                m_apply_retry_timer,
                APP_TIMER_TICKS(20U),
                NULL
            );

            return;
        }

        (void)app_timer_stop(m_apply_retry_timer);
        m_apply_pending = false;

        m_rotate_pending =
            false;

        arm_rotation_timer();
    }
}

nrfclaw_tracking_status_t
nrfclaw_tracking_config(uint16_t interval_ms,
                        int8_t tx_power_dbm,
                        uint32_t rotation_seconds)
{
    if (m_active)
        return NRFCLAW_TRACKING_BUSY;

    if (interval_ms < 100U ||
        interval_ms > 10000U ||
        rotation_seconds == 0U)
        return NRFCLAW_TRACKING_BAD_ARG;

    /*
     * nRF52832/S132 accepted discrete values are validated by SoftDevice.
     * Stage 8.2 default is +4 dBm.
     */
    m_interval_ms = interval_ms;
    m_tx_power_dbm = tx_power_dbm;
    m_rotation_seconds = rotation_seconds;

    if (m_store)
    {
        if (m_commit_state ==
            COMMIT_IDLE)
        {
            memcpy(
                &m_stage,
                m_store,
                sizeof(m_stage)
            );

            m_stage.generation =
                m_store->generation + 1U;

            m_commit_addr =
                (m_active_slot == 0U)
                ? NRFCLAW_TRACKING_SLOT_B_ADDR
                : NRFCLAW_TRACKING_SLOT_A_ADDR;
        }

        /*
         * CONFIG and ROTATION_SET can execute back-to-back in one VM tick.
         * Always update the same staged record instead of re-copying stale
         * Flash contents and losing the previous opcode's changes.
         */
        m_stage.rotation_seconds =
            rotation_seconds;

        m_stage.adv_interval_ms =
            interval_ms;

        m_stage.tx_power_dbm =
            tx_power_dbm;

        m_stage.crc32 =
            store_crc(&m_stage);

        if (m_commit_state ==
            COMMIT_IDLE)
        {
            m_commit_state =
                COMMIT_ERASE_PENDING;
        }
    }

    return NRFCLAW_TRACKING_OK;
}


nrfclaw_tracking_status_t
nrfclaw_tracking_rotation_set(uint32_t rotation_seconds)
{
    if (rotation_seconds == 0U)
        return NRFCLAW_TRACKING_BAD_ARG;

    if (m_active)
        return NRFCLAW_TRACKING_BUSY;

    m_rotation_seconds = rotation_seconds;

    if (m_store)
    {
        if (m_commit_state ==
            COMMIT_IDLE)
        {
            memcpy(
                &m_stage,
                m_store,
                sizeof(m_stage)
            );

            m_stage.generation =
                m_store->generation + 1U;

            m_commit_addr =
                (m_active_slot == 0U)
                ? NRFCLAW_TRACKING_SLOT_B_ADDR
                : NRFCLAW_TRACKING_SLOT_A_ADDR;
        }

        m_stage.rotation_seconds =
            rotation_seconds;

        m_stage.adv_interval_ms =
            m_interval_ms;

        m_stage.tx_power_dbm =
            m_tx_power_dbm;

        m_stage.crc32 =
            store_crc(&m_stage);

        if (m_commit_state ==
            COMMIT_IDLE)
        {
            m_commit_state =
                COMMIT_ERASE_PENDING;
        }
    }

    return NRFCLAW_TRACKING_OK;
}


nrfclaw_tracking_status_t nrfclaw_tracking_start(void)
{
    if (m_active ||
        m_start_pending)
    {
        return NRFCLAW_TRACKING_OK;
    }

    if (nrfclaw_ble_app_connected())
        return NRFCLAW_TRACKING_BUSY;

    /* r3.8.14b6: TRACKING owns the shared BLE advertising set while active.
     *
     * The Application/NDP adaptive advertiser can otherwise remain armed
     * (FAST/NORMAL/SLOW) after a VM motion event.  TRACKING may successfully
     * start, but the still-running adaptive timer later stops/reconfigures the
     * same shared advertising handle back to NDP, making TRACKING appear to
     * have never started.
     *
     * Claim BLE ownership here, in the native TRACKING subsystem rather than
     * in the textual compiler/VM.  This makes every TRACKING_START path
     * (text VM, legacy VM, CLI/native caller) obey the same ownership rule.
     * ROLE_OFF stops current Application advertising and disables its
     * adaptive timer, while the physical P0.21/NUS programming plane remains
     * independent.
     */
    nrfclaw_ble_app_status_t app_st =
        nrfclaw_ble_app_set_role(NRFCLAW_BLE_APP_OFF);

    if (app_st == NRFCLAW_BLE_APP_BUSY)
        return NRFCLAW_TRACKING_BUSY;

    if (app_st != NRFCLAW_BLE_APP_OK)
        return NRFCLAW_TRACKING_ERROR;

    ((void)0);

    /*
     * Manual compatibility mode keeps Stage-8.2 behavior. It is intended for
     * bench testing only and is not part of autonomous monotonic rotation.
     */
    if (m_manual_key_valid)
    {
        memcpy(
            m_adv_key,
            m_manual_key,
            sizeof(m_adv_key)
        );

        nrfclaw_tracking_status_t st =
            apply_advertising();

        if (st != NRFCLAW_TRACKING_OK)
            return st;

        m_active =
            true;

        arm_rotation_timer();

        return NRFCLAW_TRACKING_OK;
    }

    /*
     * First autonomous use generates the seed, but Stage 8.3 intentionally
     * waits for its A/B commit before any derived public key is advertised.
     */
    if (!m_store)
    {
        if (!identity_create())
            return NRFCLAW_TRACKING_ERROR;
    }

    m_start_pending =
        true;

    return NRFCLAW_TRACKING_OK;
}


void nrfclaw_tracking_stop(void)
{
    (void)app_timer_stop(m_apply_retry_timer);
    (void)app_timer_stop(m_rotation_timer);
    m_rotate_pending = false;
    m_start_pending = false;
    m_index_commit_pending = false;
    m_apply_pending = false;
    m_rotation_remaining_s = 0U;

    uint8_t adv_handle =
        nrfclaw_ble_shared_adv_handle();

    if (adv_handle != BLE_GAP_ADV_SET_HANDLE_NOT_SET)
        (void)sd_ble_gap_adv_stop(adv_handle);

    if (m_normal_addr_valid)
        (void)sd_ble_gap_addr_set(&m_normal_addr);

    m_active = false;
}

bool nrfclaw_tracking_active(void)
{
    return m_active;
}

bool nrfclaw_tracking_has_identity(void)
{
    return m_store != NULL;
}


void nrfclaw_tracking_info(nrfclaw_tracking_info_t *info)
{
    if (!info)
        return;

    memset(info, 0, sizeof(*info));

    info->identity_valid = (m_store != NULL);
    info->active = m_active;
    info->active_slot = m_active_slot;
    info->generation = m_store ? m_store->generation : 0U;
    info->key_index = m_key_index;
    info->rotation_seconds = m_rotation_seconds;
    info->adv_interval_ms = m_interval_ms;
    info->tx_power_dbm = m_tx_power_dbm;
}

bool nrfclaw_tracking_identity_read(
    uint8_t seed[NRFCLAW_TRACKING_SEED_SIZE])
{
    if (!seed || !m_store)
        return false;

    memcpy(seed, m_store->seed, NRFCLAW_TRACKING_SEED_SIZE);
    return true;
}

nrfclaw_tracking_status_t
nrfclaw_tracking_set_manual_key(
    uint8_t const key[NRFCLAW_TRACKING_KEY_SIZE])
{
    if (!key)
        return NRFCLAW_TRACKING_BAD_ARG;

    if (m_active)
        return NRFCLAW_TRACKING_BUSY;

    memcpy(m_manual_key, key, sizeof(m_manual_key));
    m_manual_key_valid = true;

    return NRFCLAW_TRACKING_OK;
}
