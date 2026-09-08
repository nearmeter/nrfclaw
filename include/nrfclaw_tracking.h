#ifndef NRFCLAW_TRACKING_H
#define NRFCLAW_TRACKING_H

#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_TRACKING_KEY_SIZE             28U
#define NRFCLAW_TRACKING_SEED_SIZE            32U
#define NRFCLAW_TRACKING_DEFAULT_INTERVAL_MS  1000U
#define NRFCLAW_TRACKING_DEFAULT_ROTATION_S   10800UL
#define NRFCLAW_TRACKING_DEFAULT_TX_DBM       4

#define NRFCLAW_TRACKING_SLOT_A_ADDR          0x00074000UL
#define NRFCLAW_TRACKING_SLOT_B_ADDR          0x00075000UL
#define NRFCLAW_TRACKING_JOURNAL_A_ADDR       0x00076000UL
#define NRFCLAW_TRACKING_JOURNAL_B_ADDR       0x00077000UL

typedef enum
{
    NRFCLAW_TRACKING_OK = 0,
    NRFCLAW_TRACKING_BUSY,
    NRFCLAW_TRACKING_BAD_ARG,
    NRFCLAW_TRACKING_NO_IDENTITY,
    NRFCLAW_TRACKING_ERROR
} nrfclaw_tracking_status_t;

typedef struct
{
    bool identity_valid;
    bool active;
    uint8_t active_slot;
    uint32_t generation;
    uint32_t key_index;
    uint32_t rotation_seconds;
    uint16_t adv_interval_ms;
    int8_t tx_power_dbm;
} nrfclaw_tracking_info_t;


typedef enum
{
    NRFCLAW_TRACK_DBG_NONE = 0,
    NRFCLAW_TRACK_DBG_START_REQUESTED,
    NRFCLAW_TRACK_DBG_JOURNAL_PENDING,
    NRFCLAW_TRACK_DBG_INDEX_COMMITTED,
    NRFCLAW_TRACK_DBG_KEY_DERIVED,
    NRFCLAW_TRACK_DBG_ADV_STOP,
    NRFCLAW_TRACK_DBG_ADDR_SET,
    NRFCLAW_TRACK_DBG_ADV_CONFIGURE,
    NRFCLAW_TRACK_DBG_TX_POWER,
    NRFCLAW_TRACK_DBG_ADV_START,
    NRFCLAW_TRACK_DBG_ACTIVE
} nrfclaw_tracking_debug_stage_t;

typedef struct
{
    uint8_t stage;
    uint8_t start_pending;
    uint8_t index_commit_pending;
    uint8_t apply_pending;
    uint8_t active;
    uint8_t commit_state;
    uint8_t journal_state;
    uint8_t reserved;
    uint32_t key_index;
    uint32_t last_sd_error;
} nrfclaw_tracking_debug_t;

void nrfclaw_tracking_debug(nrfclaw_tracking_debug_t *debug);

void nrfclaw_tracking_init(void);
void nrfclaw_tracking_process(void);

/*
 * Normal Stage 8.2 flow:
 * - first start automatically creates/persists a master seed;
 * - public advertisement keys are derived internally;
 * - no external key file is needed.
 */
nrfclaw_tracking_status_t nrfclaw_tracking_start(void);
void nrfclaw_tracking_stop(void);

nrfclaw_tracking_status_t
nrfclaw_tracking_config(uint16_t interval_ms,
                        int8_t tx_power_dbm,
                        uint32_t rotation_seconds);

nrfclaw_tracking_status_t
nrfclaw_tracking_rotation_set(uint32_t rotation_seconds);

bool nrfclaw_tracking_active(void);
bool nrfclaw_tracking_has_identity(void);

void nrfclaw_tracking_info(nrfclaw_tracking_info_t *info);

/*
 * Export is intentionally available only to the physical programming/NUS
 * plane. The seed is required by an authorized backend to derive the same
 * P-224 private-key sequence and decrypt OpenHaystack reports.
 */
bool nrfclaw_tracking_identity_read(uint8_t seed[NRFCLAW_TRACKING_SEED_SIZE]);

/*
 * Advanced compatibility mode: manually override one advertisement key.
 * This is retained for OpenHaystack/heystack testing, but it is not the
 * normal deployment path anymore.
 */
nrfclaw_tracking_status_t
nrfclaw_tracking_set_manual_key(
    uint8_t const key[NRFCLAW_TRACKING_KEY_SIZE]);

#endif
