#include "nrfclaw_direct_hall_control.h"

#include "nrfclaw_board.h"
#include "nrfclaw_native.h"
#include "nrfclaw_state.h"

#define NRFCLAW_STATE_KEY_DIRECT_HALL_CONTROL 20U
#define DIRECT_HALL_MAGIC        0x48430100UL /* "HC", config v1 */
#define DIRECT_HALL_MAGIC_MASK   0xFFFFFFF0UL
#define DIRECT_HALL_MODE_MASK    0x00000003UL
#define DIRECT_HALL_CHANNEL_MASK 0x0000000CUL
#define DIRECT_HALL_CHANNEL_SHIFT 2U

static bool m_has_persisted;
static nrfclaw_hall_mode_t m_persisted_mode = NRFCLAW_HALL_MODE_DISABLED;
static uint8_t m_persisted_channel = 1U;

static bool valid_profile(nrfclaw_hall_mode_t mode, uint8_t channel)
{
    if (mode == NRFCLAW_HALL_MODE_DISABLED)
        return true;
    if (mode == NRFCLAW_HALL_MODE_SINGLE)
        return channel == 1U || channel == 2U;
    if (mode == NRFCLAW_HALL_MODE_QUADRATURE)
        return true;
    return false;
}

static uint32_t encode(nrfclaw_hall_mode_t mode, uint8_t channel)
{
    if (mode == NRFCLAW_HALL_MODE_QUADRATURE ||
        mode == NRFCLAW_HALL_MODE_DISABLED)
        channel = 1U;
    return DIRECT_HALL_MAGIC |
           ((uint32_t)mode & DIRECT_HALL_MODE_MASK) |
           (((uint32_t)channel << DIRECT_HALL_CHANNEL_SHIFT) &
            DIRECT_HALL_CHANNEL_MASK);
}

static bool apply_profile(nrfclaw_hall_mode_t mode, uint8_t channel)
{
#if NRFCLAW_BOARD_HAS_HALL
    nrfclaw_hall_config_t cfg;

    if (mode == NRFCLAW_HALL_MODE_DISABLED) {
        nrfclaw_hall_disable();
        return true;
    }

    if (!valid_profile(mode, channel))
        return false;

    cfg.mode = mode;
    cfg.channel = mode == NRFCLAW_HALL_MODE_SINGLE ? channel : 1U;
    cfg.pullup = false;
    cfg.count_edges = true;
    cfg.emit_events = true;
    return nrfclaw_native_hall_configure(&cfg) == NRFCLAW_NATIVE_OK;
#else
    (void)mode;
    (void)channel;
    return false;
#endif
}

void nrfclaw_direct_hall_control_init(void)
{
    uint32_t value = 0U;
    uint32_t mode;
    uint8_t channel;

    m_has_persisted = false;
    m_persisted_mode = NRFCLAW_HALL_MODE_DISABLED;
    m_persisted_channel = 1U;

    if (!nrfclaw_state_get(NRFCLAW_STATE_KEY_DIRECT_HALL_CONTROL, &value))
        return;
    if ((value & DIRECT_HALL_MAGIC_MASK) != DIRECT_HALL_MAGIC)
        return;

    mode = value & DIRECT_HALL_MODE_MASK;
    channel = (uint8_t)((value & DIRECT_HALL_CHANNEL_MASK) >>
                        DIRECT_HALL_CHANNEL_SHIFT);
    if (channel == 0U)
        channel = 1U;
    if (!valid_profile((nrfclaw_hall_mode_t)mode, channel))
        return;

    m_persisted_mode = (nrfclaw_hall_mode_t)mode;
    m_persisted_channel = channel;
    m_has_persisted = true;
}

bool nrfclaw_direct_hall_control_apply_persisted(void)
{
    if (!m_has_persisted)
        return true;
    return apply_profile(m_persisted_mode, m_persisted_channel);
}

bool nrfclaw_direct_hall_control_set_persist(nrfclaw_hall_mode_t mode,
                                             uint8_t channel)
{
#if !NRFCLAW_BOARD_HAS_HALL
    (void)mode;
    (void)channel;
    return false;
#else
    if (!valid_profile(mode, channel))
        return false;

    if (mode == NRFCLAW_HALL_MODE_DISABLED ||
        mode == NRFCLAW_HALL_MODE_QUADRATURE)
        channel = 1U;

    if (m_has_persisted && m_persisted_mode == mode &&
        m_persisted_channel == channel)
        return apply_profile(mode, channel);

    if (nrfclaw_state_status() != NRFCLAW_STATE_IDLE)
        return false;
    if (!nrfclaw_state_persist(
            NRFCLAW_STATE_KEY_DIRECT_HALL_CONTROL,
            encode(mode, channel)))
        return false;

    m_persisted_mode = mode;
    m_persisted_channel = channel;
    m_has_persisted = true;
    return apply_profile(mode, channel);
#endif
}

nrfclaw_hall_mode_t nrfclaw_direct_hall_control_runtime_mode(void)
{
#if NRFCLAW_BOARD_HAS_HALL
    return nrfclaw_hall_mode();
#else
    return NRFCLAW_HALL_MODE_DISABLED;
#endif
}

uint8_t nrfclaw_direct_hall_control_runtime_channel(void)
{
#if NRFCLAW_BOARD_HAS_HALL
    return nrfclaw_hall_channel();
#else
    return 1U;
#endif
}

nrfclaw_hall_mode_t nrfclaw_direct_hall_control_persisted_mode(void)
{
    return m_persisted_mode;
}

uint8_t nrfclaw_direct_hall_control_persisted_channel(void)
{
    return m_persisted_channel;
}

bool nrfclaw_direct_hall_control_has_persisted(void)
{
    return m_has_persisted;
}

uint8_t nrfclaw_direct_hall_control_store_status(void)
{
    return (uint8_t)nrfclaw_state_status();
}

bool nrfclaw_direct_hall_control_reset_value(void)
{
#if NRFCLAW_BOARD_HAS_HALL
    if (!nrfclaw_hall_active())
        return false;
    nrfclaw_hall_reset_value();
    return true;
#else
    return false;
#endif
}
