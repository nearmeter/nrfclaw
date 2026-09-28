#include "nrfclaw_ninalink_query.h"
#include "nrfclaw_capability.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_command.h"
#include "nrfclaw_ninalink_command_registry.h"
#include "nrfclaw_ninalink_msg.h"
#include "nrfclaw_ninalink_state_cache.h"
#include <string.h>

static nrfclaw_ninalink_query_status_t m_status;
static nrfclaw_ninalink_query_endpoint_t m_endpoints[NRFCLAW_NINALINK_QUERY_ENDPOINT_MAX];

static bool value_from_raw(uint8_t value_type, uint32_t raw, nrfclaw_capability_value_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->type = value_type;
    switch (value_type) {
        case NRFCLAW_CAP_VALUE_BOOL:
            if (raw > 1U) return false;
            out->v.boolean = raw != 0U; return true;
        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_ENUM8:
            if (raw > 0xFFU) return false;
            out->v.u8 = (uint8_t)raw; return true;
        case NRFCLAW_CAP_VALUE_S8:
            out->v.s8 = (int8_t)(uint8_t)raw; return true;
        case NRFCLAW_CAP_VALUE_U16:
            if (raw > 0xFFFFU) return false;
            out->v.u16 = (uint16_t)raw; return true;
        case NRFCLAW_CAP_VALUE_S16:
            out->v.s16 = (int16_t)(uint16_t)raw; return true;
        case NRFCLAW_CAP_VALUE_U32:
            out->v.u32 = raw; return true;
        case NRFCLAW_CAP_VALUE_S32:
            out->v.s32 = (int32_t)raw; return true;
        default:
            return false;
    }
}

void nrfclaw_ninalink_query_reset(void)
{
    memset(&m_status, 0, sizeof(m_status));
    memset(m_endpoints, 0, sizeof(m_endpoints));
    m_status.state = NRFCLAW_NINALINK_QUERY_IDLE;
    m_status.command_result = 0xFFU;
    m_status.item_status[0] = 0xFFU;
    m_status.item_status[1] = 0xFFU;
}

bool nrfclaw_ninalink_query_queue(uint32_t target_node,
                                  const nrfclaw_ninalink_query_endpoint_t *endpoints,
                                  uint8_t count)
{
    nrfclaw_ninalink_command_dl_status_t cs;
    uint8_t args[7];
    uint8_t i, arg_len = 1U;

    if (target_node == 0U || target_node == 0xFFFFFFFFUL ||
        !endpoints || count == 0U || count > NRFCLAW_NINALINK_QUERY_ENDPOINT_MAX)
        return false;
    if (m_status.state == NRFCLAW_NINALINK_QUERY_PENDING)
        return false;

    memset(args, 0, sizeof(args));
    args[0] = count;
    for (i = 0U; i < count; i++) {
        args[arg_len++] = (uint8_t)endpoints[i].capability_id;
        args[arg_len++] = (uint8_t)(endpoints[i].capability_id >> 8);
        args[arg_len++] = endpoints[i].channel;
    }

    if (!nrfclaw_ninalink_bridge_queue_command(
            target_node, NRFCLAW_NINALINK_COMMAND_QUERY_CAPABILITIES,
            args, arg_len))
        return false;

    memset(&cs, 0, sizeof(cs));
    nrfclaw_ninalink_bridge_get_command_status(&cs);
    memset(&m_status, 0, sizeof(m_status));
    memset(m_endpoints, 0, sizeof(m_endpoints));
    m_status.state = NRFCLAW_NINALINK_QUERY_PENDING;
    m_status.count = count;
    m_status.target_node = target_node;
    m_status.command_seq = cs.command_seq;
    m_status.command_result = 0xFFU;
    m_status.item_status[0] = 0xFFU;
    m_status.item_status[1] = 0xFFU;
    memcpy(m_endpoints, endpoints, (size_t)count * sizeof(endpoints[0]));
    return true;
}

void nrfclaw_ninalink_query_get_status(nrfclaw_ninalink_query_status_t *out)
{
    if (out) *out = m_status;
}

void nrfclaw_ninalink_query_on_command_result(uint32_t target_node,
                                               uint16_t command_seq,
                                               uint16_t command_id,
                                               uint8_t command_result,
                                               const uint8_t *result_data,
                                               uint8_t result_len,
                                               uint32_t now_s)
{
    nrfclaw_ninalink_value_entry_t entries[NRFCLAW_NINALINK_QUERY_ENDPOINT_MAX];
    uint8_t cached_count = 0U, i;

    if (m_status.state != NRFCLAW_NINALINK_QUERY_PENDING ||
        target_node != m_status.target_node ||
        command_seq != m_status.command_seq ||
        command_id != NRFCLAW_NINALINK_COMMAND_QUERY_CAPABILITIES)
        return;

    m_status.command_result = command_result;
    if (command_result != NRFCLAW_NINALINK_COMMAND_STATUS_OK) {
        m_status.state = NRFCLAW_NINALINK_QUERY_ERROR;
        return;
    }
    if (!result_data || result_len != (uint8_t)(1U + 6U * m_status.count) ||
        result_data[0] != m_status.count) {
        m_status.state = NRFCLAW_NINALINK_QUERY_ERROR;
        return;
    }

    memset(entries, 0, sizeof(entries));
    for (i = 0U; i < m_status.count; i++) {
        uint8_t off = (uint8_t)(1U + 6U * i);
        uint8_t item_status = result_data[off];
        uint8_t value_type = result_data[off + 1U];
        uint32_t raw = (uint32_t)result_data[off + 2U] |
                       ((uint32_t)result_data[off + 3U] << 8) |
                       ((uint32_t)result_data[off + 4U] << 16) |
                       ((uint32_t)result_data[off + 5U] << 24);
        m_status.item_status[i] = item_status;
        if (item_status != NRFCLAW_NINALINK_QUERY_ITEM_OK)
            continue;
        entries[cached_count].capability_id = m_endpoints[i].capability_id;
        entries[cached_count].channel = m_endpoints[i].channel;
        if (!value_from_raw(value_type, raw, &entries[cached_count].value)) {
            m_status.item_status[i] = NRFCLAW_NINALINK_QUERY_ITEM_UNAVAILABLE;
            continue;
        }
        cached_count++;
    }

    if (cached_count != 0U &&
        !nrfclaw_ninalink_state_cache_upsert_query_values(
            target_node, entries, cached_count, now_s)) {
        m_status.state = NRFCLAW_NINALINK_QUERY_ERROR;
        return;
    }

    m_status.cached_count = cached_count;
    m_status.state = NRFCLAW_NINALINK_QUERY_DONE;
}
