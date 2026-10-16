#include "nrfclaw_rf_receiver_boot.h"

#include "nrfclaw_rf_profile_registry.h" /* B8.0m1 */
#include "nrfclaw_state.h"
#include "nrfclaw_llcc68_rl.h"
#include "nrfclaw_rf_ingest.h"
#include "nrfclaw_rfvm.h"

#include <string.h>

#define NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT 22U

#define RF_BOOT_ENABLE_BIT       0x80000000UL
#define RF_BOOT_MAGIC_MASK       0x70000000UL
#define RF_BOOT_MAGIC_V1         0x50000000UL
#define RF_BOOT_MAGIC_V2         0x60000000UL
#define RF_BOOT_PROFILE_SHIFT    24U
#define RF_BOOT_PROFILE_MASK     0x0F000000UL
#define RF_BOOT_PROFILE_HI_SHIFT 20U
#define RF_BOOT_PROFILE_HI_MASK  0x00F00000UL
#define RF_BOOT_FREQ_KHZ_MASK    0x000FFFFFUL

#define RF_BOOT_DEFAULT_PROFILE  NRFCLAW_RF_PROFILE_VEVOR_YT60234
#define RF_BOOT_DEFAULT_FREQ_HZ  915000000UL
#define RF_BOOT_DEFAULT_BW       NRFCLAW_LLCC68_GFSK_BW_117_3

static bool m_persisted;
static bool m_enabled;
static uint8_t m_profile_id;
static uint8_t m_bw_code;
static uint32_t m_frequency_hz;

static bool config_valid(uint8_t profile_id,
                         uint32_t frequency_hz,
                         uint8_t bw_code)
{
    const nrfclaw_rf_profile_entry_t *e;
    uint32_t khz = frequency_hz / 1000UL;

    /*
     * B8.1g-r4: the registry owns the PHY. Persistence selects a logical
     * runtime-ready profile plus one exact center frequency; callers may not
     * silently substitute a different LLCC68 RX bandwidth.
     */
    e = nrfclaw_rf_profile_registry_get(profile_id);
    if (!e || !nrfclaw_rf_profile_registry_runtime_ready(profile_id))
        return false;

    /* Generic LLCC68 chip-level sanity. Populated 400/900 module capability
     * remains a separate policy gate. */
    if (frequency_hz < 150000000UL || frequency_hz > 960000000UL)
        return false;
    if ((frequency_hz % 1000UL) != 0UL || khz > RF_BOOT_FREQ_KHZ_MASK)
        return false;

    if (bw_code != e->rx_bw_code)
        return false;

    return true;
}

static uint32_t encode(bool enabled)
{
    uint32_t khz = m_frequency_hz / 1000UL;

    /* B8.1j persistence v2:
     * bits 27:24 = profile low nibble
     * bits 23:20 = profile high nibble
     * v1 used bits 23:20 as a BW compatibility token.
     */
    return (enabled ? RF_BOOT_ENABLE_BIT : 0UL) |
           RF_BOOT_MAGIC_V2 |
           (((uint32_t)m_profile_id << RF_BOOT_PROFILE_SHIFT) &
            RF_BOOT_PROFILE_MASK) |
           (((uint32_t)m_profile_id << (RF_BOOT_PROFILE_HI_SHIFT - 4U)) &
            RF_BOOT_PROFILE_HI_MASK) |
           (khz & RF_BOOT_FREQ_KHZ_MASK);
}

void nrfclaw_rf_receiver_boot_init(void)
{
    uint32_t v = 0UL;
    uint32_t magic;

    m_persisted = false;
    m_enabled = false;
    m_profile_id = RF_BOOT_DEFAULT_PROFILE;
    m_bw_code = RF_BOOT_DEFAULT_BW;
    m_frequency_hz = RF_BOOT_DEFAULT_FREQ_HZ;

    if (!nrfclaw_state_get(NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT, &v))
        return;

    magic = v & RF_BOOT_MAGIC_MASK;
    if (magic != RF_BOOT_MAGIC_V1 && magic != RF_BOOT_MAGIC_V2)
        return;

    {
        uint8_t profile =
            (uint8_t)((v & RF_BOOT_PROFILE_MASK) >> RF_BOOT_PROFILE_SHIFT);
        uint32_t frequency_hz =
            (v & RF_BOOT_FREQ_KHZ_MASK) * 1000UL;
        const nrfclaw_rf_profile_entry_t *e;
        uint8_t bw;

        /* B8.1j: v1 decodes the original 4-bit profile. v2 reuses the
         * obsolete BW token nibble as profile bits 7:4.
         */
        if (magic == RF_BOOT_MAGIC_V2) {
            profile |= (uint8_t)(
                (v & RF_BOOT_PROFILE_HI_MASK) >>
                (RF_BOOT_PROFILE_HI_SHIFT - 4U));
        }

        e = nrfclaw_rf_profile_registry_get(profile);
        if (!e || !nrfclaw_rf_profile_registry_runtime_ready(profile))
            return;

        /* Registry remains authoritative for the complete LLCC68 BW code. */
        bw = e->rx_bw_code;

        if (!config_valid(profile, frequency_hz, bw))
            return;

        m_profile_id = profile;
        m_bw_code = bw;
        m_frequency_hz = frequency_hz;
        m_enabled = (v & RF_BOOT_ENABLE_BIT) != 0UL;
        m_persisted = true;
    }
}

void nrfclaw_rf_receiver_boot_note_runtime(uint8_t profile_id,
                                           uint32_t frequency_hz,
                                           uint8_t bw_code)
{
    if (!config_valid(profile_id, frequency_hz, bw_code))
        return;
    m_profile_id = profile_id;
    m_frequency_hz = frequency_hz;
    m_bw_code = bw_code;
}

bool nrfclaw_rf_receiver_boot_start_saved(void)
{
    if (!m_enabled ||
        !config_valid(m_profile_id, m_frequency_hz, m_bw_code))
        return false;

    {
        const nrfclaw_rf_profile_entry_t *e =
            nrfclaw_rf_profile_registry_get(m_profile_id);
        nrfclaw_llcc68_gfsk_profile_t cfg;
        if(!e || !nrfclaw_rf_profile_registry_runtime_ready(m_profile_id))
            return false;
        memset(&cfg,0,sizeof(cfg));
        cfg.frequency_hz=m_frequency_hz;
        cfg.bitrate_bps=e->bitrate_bps;
        cfg.deviation_hz=e->deviation_hz;
        cfg.rx_bw_code=m_bw_code;
        cfg.preamble_bits=e->preamble_bits;
        memcpy(cfg.sync,e->sync,sizeof(cfg.sync));
        cfg.sync_len=e->sync_len;
        /* B8.1k6c variable packet mode: registry flag owns framing. */
        cfg.packet_mode=
            (e->flags&NRFCLAW_RF_PROFILE_FLAG_VARIABLE_PACKET)?
            NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE:
            NRFCLAW_LLCC68_GFSK_PACKET_FIXED;
        cfg.payload_len=
            (cfg.packet_mode==NRFCLAW_LLCC68_GFSK_PACKET_VARIABLE)?
            255U:e->expected_len;
        if(!nrfclaw_llcc68_rl_gfsk_start_profile(&cfg))
            return false;
    }

    if (!nrfclaw_rf_ingest_start(m_profile_id)) {
        (void)nrfclaw_llcc68_rl_gfsk_stop_restore_lora();
        return false;
    }
    return true;
}

bool nrfclaw_rf_receiver_boot_enable_persist(void)
{
    if (!config_valid(m_profile_id, m_frequency_hz, m_bw_code))
        return false;
    if (m_persisted && m_enabled)
        return true;
    if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;
    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT, encode(true)))
        return false;
    m_persisted = true;
    m_enabled = true;
    return true;
}

bool nrfclaw_rf_receiver_boot_disable_persist(void)
{
    if (m_persisted && !m_enabled)
        return true;
    if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;
    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT, encode(false)))
        return false;
    m_persisted = true;
    m_enabled = false;
    return true;
}

bool nrfclaw_rf_receiver_boot_enabled(void)
{
    return m_persisted && m_enabled;
}

/* B8.0l canonical exclusive bridge-radio mode.
 * B8.0k2 flash compatibility is preserved:
 * enabled=0 => native NinaLink LoRa bridge
 * enabled=1 => external GFSK/RFVM receiver. */
nrfclaw_bridge_radio_mode_t nrfclaw_bridge_radio_mode_get(void)
{
    return (m_persisted && m_enabled)
        ? NRFCLAW_BRIDGE_RADIO_MODE_EXTERNAL_RF
        : NRFCLAW_BRIDGE_RADIO_MODE_LORA;
}

bool nrfclaw_bridge_radio_mode_is_external(void)
{
    return nrfclaw_bridge_radio_mode_get() ==
           NRFCLAW_BRIDGE_RADIO_MODE_EXTERNAL_RF;
}

bool nrfclaw_bridge_radio_mode_set_lora_persist(void)
{
    return nrfclaw_rf_receiver_boot_disable_persist();
}

bool nrfclaw_bridge_radio_mode_set_external_persist(
    uint8_t profile_id, uint32_t frequency_hz, uint8_t bw_code)
{
    /* B8.0m1 runtime-ready profile gate. */
    if (!nrfclaw_rf_profile_registry_runtime_ready(profile_id))
        return false;

    uint32_t current = 0UL;
    uint32_t wanted;
    if (!config_valid(profile_id, frequency_hz, bw_code))
        return false;
    m_profile_id = profile_id;
    m_frequency_hz = frequency_hz;
    m_bw_code = bw_code;
    wanted = encode(true);
    if (m_persisted && m_enabled &&
        nrfclaw_state_get(NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT, &current) &&
        current == wanted)
        return true;
    if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;
    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_RF_RECEIVER_BOOT, wanted))
        return false;
    m_persisted = true;
    m_enabled = true;
    return true;
}

void nrfclaw_rf_receiver_boot_get_status(
    nrfclaw_rf_receiver_boot_status_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->persisted = m_persisted;
    out->enabled = m_enabled;
    out->runtime_active =
        nrfclaw_rf_ingest_active() &&
        nrfclaw_llcc68_rl_gfsk_active();
    out->profile_id = m_profile_id;
    out->bw_code = m_bw_code;
    out->frequency_hz = m_frequency_hz;
    out->store_status = (uint8_t)nrfclaw_state_status();
}
