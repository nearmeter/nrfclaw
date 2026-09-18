#include "nrfclaw_ninalink_command_registry.h"

#include <string.h>

typedef uint8_t (*command_handler_t)(
    const nrfclaw_ninalink_command_context_t *context,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len);

typedef struct {
    nrfclaw_ninalink_command_descriptor_t descriptor;
    command_handler_t handler;
} command_entry_t;

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint8_t command_echo_u32(
    const nrfclaw_ninalink_command_context_t *context,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len)
{
    (void)context;
    (void)arg_len;
    memcpy(result, args, 4U);
    *result_len = 4U;
    return NRFCLAW_NINALINK_COMMAND_STATUS_OK;
}

static uint8_t command_get_node_info(
    const nrfclaw_ninalink_command_context_t *context,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len)
{
    uint8_t feature_flags;

    (void)args;
    (void)arg_len;

    if (!context)
        return NRFCLAW_NINALINK_COMMAND_STATUS_EXEC_FAILED;

    feature_flags =
        NRFCLAW_NINALINK_NODE_FEATURE_CAP_SET |
        NRFCLAW_NINALINK_NODE_FEATURE_RELIABLE_COMMAND |
        NRFCLAW_NINALINK_NODE_FEATURE_COMMAND_REGISTRY |
        NRFCLAW_NINALINK_NODE_FEATURE_TRACKING;

    result[0] = 1U;
    result[1] = 64U;
    result[2] = nrfclaw_ninalink_command_registry_count();
    result[3] = feature_flags;
    put_u32_le(&result[4], context->node_id);
    *result_len = 8U;

    return NRFCLAW_NINALINK_COMMAND_STATUS_OK;
}

static uint8_t command_get_tracking_state(
    const nrfclaw_ninalink_command_context_t *context,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len)
{
    (void)args;
    (void)arg_len;

    if (!context)
        return NRFCLAW_NINALINK_COMMAND_STATUS_EXEC_FAILED;

    result[0] = context->tracking_active ? 1U : 0U;
    *result_len = 1U;
    return NRFCLAW_NINALINK_COMMAND_STATUS_OK;
}

static const command_entry_t m_registry[] = {
    {
        { NRFCLAW_NINALINK_COMMAND_ECHO_U32,
          4U, 4U, 4U, NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY },
        command_echo_u32
    },
    {
        { NRFCLAW_NINALINK_COMMAND_GET_NODE_INFO,
          0U, 0U, 8U, NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY },
        command_get_node_info
    },
    {
        { NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE,
          0U, 0U, 1U, NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY },
        command_get_tracking_state
    }
};

uint8_t nrfclaw_ninalink_command_registry_count(void)
{
    return (uint8_t)(sizeof(m_registry) / sizeof(m_registry[0]));
}

bool nrfclaw_ninalink_command_registry_get(
    uint8_t index,
    nrfclaw_ninalink_command_descriptor_t *out)
{
    if (!out || index >= nrfclaw_ninalink_command_registry_count())
        return false;

    *out = m_registry[index].descriptor;
    return true;
}

uint8_t nrfclaw_ninalink_command_registry_execute(
    const nrfclaw_ninalink_command_context_t *context,
    uint16_t command_id,
    const uint8_t *args,
    uint8_t arg_len,
    uint8_t *result,
    uint8_t *result_len)
{
    uint8_t i;

    if (!result || !result_len)
        return NRFCLAW_NINALINK_COMMAND_STATUS_EXEC_FAILED;

    *result_len = 0U;

    if (arg_len && !args)
        return NRFCLAW_NINALINK_COMMAND_STATUS_BAD_ARGS;

    for (i = 0U; i < nrfclaw_ninalink_command_registry_count(); i++) {
        const command_entry_t *entry = &m_registry[i];
        uint8_t status;

        if (entry->descriptor.command_id != command_id)
            continue;

        if (arg_len < entry->descriptor.min_args ||
            arg_len > entry->descriptor.max_args)
            return NRFCLAW_NINALINK_COMMAND_STATUS_BAD_ARGS;

        status = entry->handler(
            context, args, arg_len, result, result_len);

        if (*result_len > entry->descriptor.max_result ||
            *result_len > NRFCLAW_NINALINK_COMMAND_REGISTRY_RESULT_MAX) {
            *result_len = 0U;
            return NRFCLAW_NINALINK_COMMAND_STATUS_EXEC_FAILED;
        }

        return status;
    }

    return NRFCLAW_NINALINK_COMMAND_STATUS_UNSUPPORTED;
}
