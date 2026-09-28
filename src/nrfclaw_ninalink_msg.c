#include "nrfclaw_ninalink_msg.h"

#include <string.h>

static void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFUL);
    p[1] = (uint8_t)((v >> 8) & 0xFFUL);
    p[2] = (uint8_t)((v >> 16) & 0xFFUL);
    p[3] = (uint8_t)((v >> 24) & 0xFFUL);
}

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] |
                      ((uint16_t)p[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void frame_init(nrfclaw_ninalink_frame_t *frame,
                       uint8_t message_type,
                       uint16_t network_id,
                       uint32_t node_id,
                       uint16_t sequence,
                       uint8_t flags)
{
    memset(frame, 0, sizeof(*frame));
    frame->flags = flags;
    frame->message_type = message_type;
    frame->network_id = network_id;
    frame->node_id = node_id;
    frame->sequence = sequence;
}

static uint8_t value_size(uint8_t value_type)
{
    switch (value_type) {
        case NRFCLAW_CAP_VALUE_BOOL:
        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_S8:
        case NRFCLAW_CAP_VALUE_ENUM8:
            return 1U;

        case NRFCLAW_CAP_VALUE_U16:
        case NRFCLAW_CAP_VALUE_S16:
            return 2U;

        case NRFCLAW_CAP_VALUE_U32:
        case NRFCLAW_CAP_VALUE_S32:
            return 4U;

        default:
            return 0U;
    }
}

static bool put_value(uint8_t *dst,
                      const nrfclaw_capability_value_t *value)
{
    if (!dst || !value)
        return false;

    switch (value->type) {
        case NRFCLAW_CAP_VALUE_BOOL:
            if (value->v.boolean != false && value->v.boolean != true)
                return false;
            dst[0] = value->v.boolean ? 1U : 0U;
            return true;

        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_ENUM8:
            dst[0] = value->v.u8;
            return true;

        case NRFCLAW_CAP_VALUE_S8:
            dst[0] = (uint8_t)value->v.s8;
            return true;

        case NRFCLAW_CAP_VALUE_U16:
            put_u16_le(dst, value->v.u16);
            return true;

        case NRFCLAW_CAP_VALUE_S16:
            put_u16_le(dst, (uint16_t)value->v.s16);
            return true;

        case NRFCLAW_CAP_VALUE_U32:
            put_u32_le(dst, value->v.u32);
            return true;

        case NRFCLAW_CAP_VALUE_S32:
            put_u32_le(dst, (uint32_t)value->v.s32);
            return true;

        default:
            return false;
    }
}

static bool get_value(const uint8_t *src,
                      uint8_t value_type,
                      nrfclaw_capability_value_t *value)
{
    if (!src || !value)
        return false;

    memset(value, 0, sizeof(*value));
    value->type = value_type;

    switch (value_type) {
        case NRFCLAW_CAP_VALUE_BOOL:
            if (src[0] > 1U)
                return false;
            value->v.boolean = (src[0] != 0U);
            return true;

        case NRFCLAW_CAP_VALUE_U8:
        case NRFCLAW_CAP_VALUE_ENUM8:
            value->v.u8 = src[0];
            return true;

        case NRFCLAW_CAP_VALUE_S8:
            value->v.s8 = (int8_t)src[0];
            return true;

        case NRFCLAW_CAP_VALUE_U16:
            value->v.u16 = get_u16_le(src);
            return true;

        case NRFCLAW_CAP_VALUE_S16:
            value->v.s16 = (int16_t)get_u16_le(src);
            return true;

        case NRFCLAW_CAP_VALUE_U32:
            value->v.u32 = get_u32_le(src);
            return true;

        case NRFCLAW_CAP_VALUE_S32:
            value->v.s32 = (int32_t)get_u32_le(src);
            return true;

        default:
            return false;
    }
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_hello(nrfclaw_ninalink_frame_t *frame,
                             uint16_t network_id,
                             uint32_t node_id,
                             uint16_t sequence,
                             bool ack_req,
                             const nrfclaw_ninalink_hello_t *hello)
{
    uint8_t *p;

    if (!frame || !hello)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    frame_init(frame,
               NRFCLAW_NINALINK_MSG_HELLO,
               network_id,
               node_id,
               sequence,
               ack_req ? NRFCLAW_NINALINK_FLAG_ACK_REQ : 0U);

    frame->payload_length = NRFCLAW_NINALINK_HELLO_PAYLOAD_SIZE;
    p = frame->payload;

    put_u32_le(&p[0], hello->device_id0);
    put_u32_le(&p[4], hello->device_id1);
    p[8] = hello->capability_registry_version;
    p[9] = hello->advertised_capability_count;
    p[10] = hello->board_type;
    p[11] = hello->hw_rev_major;
    p[12] = hello->hw_rev_minor;
    p[13] = hello->fw_major;
    p[14] = hello->fw_minor;
    p[15] = hello->fw_patch;
    p[16] = hello->fw_prerelease;
    put_u16_le(&p[17], hello->fw_build);

    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_hello(const nrfclaw_ninalink_frame_t *frame,
                             nrfclaw_ninalink_hello_t *hello)
{
    const uint8_t *p;

    if (!frame || !hello)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_HELLO)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length != NRFCLAW_NINALINK_HELLO_PAYLOAD_SIZE)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    p = frame->payload;
    memset(hello, 0, sizeof(*hello));

    hello->device_id0 = get_u32_le(&p[0]);
    hello->device_id1 = get_u32_le(&p[4]);
    hello->capability_registry_version = p[8];
    hello->advertised_capability_count = p[9];
    hello->board_type = p[10];
    hello->hw_rev_major = p[11];
    hello->hw_rev_minor = p[12];
    hello->fw_major = p[13];
    hello->fw_minor = p[14];
    hello->fw_patch = p[15];
    hello->fw_prerelease = p[16];
    hello->fw_build = get_u16_le(&p[17]);

    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_caps_request(nrfclaw_ninalink_frame_t *frame,
                                    uint16_t network_id,
                                    uint32_t node_id,
                                    uint16_t sequence,
                                    bool ack_req,
                                    uint8_t page_index)
{
    if (!frame)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    frame_init(frame,
               NRFCLAW_NINALINK_MSG_CAPS_REQUEST,
               network_id,
               node_id,
               sequence,
               ack_req ? NRFCLAW_NINALINK_FLAG_ACK_REQ : 0U);

    frame->payload_length = 1U;
    frame->payload[0] = page_index;
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_caps_request(const nrfclaw_ninalink_frame_t *frame,
                                    uint8_t *page_index)
{
    if (!frame || !page_index)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_CAPS_REQUEST)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length != 1U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    *page_index = frame->payload[0];
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_caps_response(
    nrfclaw_ninalink_frame_t *frame,
    uint16_t network_id,
    uint32_t node_id,
    uint16_t sequence,
    bool ack_req,
    bool more,
    uint8_t registry_version,
    uint8_t page_index,
    const nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t descriptor_count)
{
    uint8_t i;
    uint8_t *p;
    uint8_t flags = 0U;

    if (!frame)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (descriptor_count > NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS)
        return NRFCLAW_NINALINK_MSG_ERR_TOO_MANY;

    if (descriptor_count != 0U && !descriptors)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (ack_req)
        flags |= NRFCLAW_NINALINK_FLAG_ACK_REQ;
    if (more)
        flags |= NRFCLAW_NINALINK_FLAG_MORE;

    frame_init(frame,
               NRFCLAW_NINALINK_MSG_CAPS_RESPONSE,
               network_id,
               node_id,
               sequence,
               flags);

    frame->payload[0] = registry_version;
    frame->payload[1] = page_index;
    frame->payload[2] = descriptor_count;
    frame->payload[3] = 0U;

    p = &frame->payload[NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE];
    for (i = 0U; i < descriptor_count; i++) {
        put_u16_le(&p[0], descriptors[i].desc.capability_id);
        p[2] = descriptors[i].desc.channel;
        p[3] = descriptors[i].desc.kind;
        p[4] = descriptors[i].desc.value_type;
        p[5] = (uint8_t)descriptors[i].desc.scale10;
        p[6] = descriptors[i].desc.unit;
        p[7] = descriptors[i].desc.behavior_flags;
        p[8] = descriptors[i].runtime_state_flags;
        p += NRFCLAW_NINALINK_CAPS_DESCRIPTOR_SIZE;
    }

    frame->payload_length =
        (uint8_t)(NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE +
                  descriptor_count * NRFCLAW_NINALINK_CAPS_DESCRIPTOR_SIZE);

    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_caps_response(
    const nrfclaw_ninalink_frame_t *frame,
    uint8_t *registry_version,
    uint8_t *page_index,
    nrfclaw_ninalink_capability_descriptor_t *descriptors,
    uint8_t descriptor_capacity,
    uint8_t *descriptor_count,
    bool *more)
{
    uint8_t count;
    uint8_t expected_length;
    uint8_t i;
    const uint8_t *p;

    if (!frame || !registry_version || !page_index ||
        !descriptor_count || !more)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_CAPS_RESPONSE)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length < NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    count = frame->payload[2];
    if (count > NRFCLAW_NINALINK_CAPS_MAX_DESCRIPTORS)
        return NRFCLAW_NINALINK_MSG_ERR_TOO_MANY;

    if (count > descriptor_capacity)
        return NRFCLAW_NINALINK_MSG_ERR_NO_SPACE;

    if (count != 0U && !descriptors)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    expected_length =
        (uint8_t)(NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE +
                  count * NRFCLAW_NINALINK_CAPS_DESCRIPTOR_SIZE);

    if (frame->payload_length != expected_length)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    if (frame->payload[3] != 0U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    *registry_version = frame->payload[0];
    *page_index = frame->payload[1];
    *descriptor_count = count;
    *more = ((frame->flags & NRFCLAW_NINALINK_FLAG_MORE) != 0U);

    p = &frame->payload[NRFCLAW_NINALINK_CAPS_PAGE_HEADER_SIZE];
    for (i = 0U; i < count; i++) {
        memset(&descriptors[i], 0, sizeof(descriptors[i]));
        descriptors[i].desc.capability_id = get_u16_le(&p[0]);
        descriptors[i].desc.channel = p[2];
        descriptors[i].desc.kind = p[3];
        descriptors[i].desc.value_type = p[4];
        descriptors[i].desc.scale10 = (int8_t)p[5];
        descriptors[i].desc.unit = p[6];
        descriptors[i].desc.behavior_flags = p[7];
        descriptors[i].runtime_state_flags = p[8];
        p += NRFCLAW_NINALINK_CAPS_DESCRIPTOR_SIZE;
    }

    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_values(
    nrfclaw_ninalink_frame_t *frame,
    uint8_t message_type,
    uint16_t network_id,
    uint32_t node_id,
    uint16_t sequence,
    bool ack_req,
    const nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_count)
{
    uint8_t i;
    uint8_t used = 1U;
    uint8_t sz;
    uint8_t *p;

    if (!frame || !entries || entry_count == 0U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (message_type != NRFCLAW_NINALINK_MSG_CAP_REPORT &&
        message_type != NRFCLAW_NINALINK_MSG_CAP_EVENT)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (entry_count > NRFCLAW_NINALINK_MAX_VALUE_ENTRIES)
        return NRFCLAW_NINALINK_MSG_ERR_TOO_MANY;

    for (i = 0U; i < entry_count; i++) {
        sz = value_size(entries[i].value.type);
        if (sz == 0U)
            return NRFCLAW_NINALINK_MSG_ERR_UNSUPPORTED_VALUE_TYPE;

        if ((uint16_t)used +
            NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE +
            sz > NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE)
            return NRFCLAW_NINALINK_MSG_ERR_NO_SPACE;

        used = (uint8_t)(used +
                         NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE +
                         sz);
    }

    frame_init(frame,
               message_type,
               network_id,
               node_id,
               sequence,
               ack_req ? NRFCLAW_NINALINK_FLAG_ACK_REQ : 0U);

    frame->payload[0] = entry_count;
    p = &frame->payload[1];

    for (i = 0U; i < entry_count; i++) {
        sz = value_size(entries[i].value.type);
        put_u16_le(&p[0], entries[i].capability_id);
        p[2] = entries[i].channel;
        p[3] = entries[i].value.type;

        if (!put_value(&p[4], &entries[i].value))
            return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

        p += (uint8_t)(NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE + sz);
    }

    frame->payload_length = used;
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_values(
    const nrfclaw_ninalink_frame_t *frame,
    nrfclaw_ninalink_value_entry_t *entries,
    uint8_t entry_capacity,
    uint8_t *entry_count)
{
    uint8_t count;
    uint8_t i;
    uint8_t offset = 1U;
    uint8_t value_type;
    uint8_t sz;

    if (!frame || !entries || !entry_count)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_CAP_REPORT &&
        frame->message_type != NRFCLAW_NINALINK_MSG_CAP_EVENT)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length < 1U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    count = frame->payload[0];
    if (count == 0U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    if (count > NRFCLAW_NINALINK_MAX_VALUE_ENTRIES)
        return NRFCLAW_NINALINK_MSG_ERR_TOO_MANY;

    if (count > entry_capacity)
        return NRFCLAW_NINALINK_MSG_ERR_NO_SPACE;

    for (i = 0U; i < count; i++) {
        if ((uint16_t)offset + NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE >
            frame->payload_length)
            return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

        value_type = frame->payload[offset + 3U];
        sz = value_size(value_type);
        if (sz == 0U)
            return NRFCLAW_NINALINK_MSG_ERR_UNSUPPORTED_VALUE_TYPE;

        if ((uint16_t)offset +
            NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE +
            sz > frame->payload_length)
            return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

        memset(&entries[i], 0, sizeof(entries[i]));
        entries[i].capability_id = get_u16_le(&frame->payload[offset]);
        entries[i].channel = frame->payload[offset + 2U];

        if (!get_value(&frame->payload[offset + 4U],
                       value_type,
                       &entries[i].value))
            return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

        offset = (uint8_t)(offset +
                           NRFCLAW_NINALINK_VALUE_ENTRY_HEADER_SIZE +
                           sz);
    }

    if (offset != frame->payload_length)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    *entry_count = count;
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_ack(nrfclaw_ninalink_frame_t *frame,
                           uint16_t network_id,
                           uint32_t node_id,
                           uint16_t sequence,
                           uint16_t acknowledged_sequence)
{
    if (!frame)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    frame_init(frame,
               NRFCLAW_NINALINK_MSG_ACK,
               network_id,
               node_id,
               sequence,
               0U);

    frame->payload_length = NRFCLAW_NINALINK_ACK_PAYLOAD_SIZE;
    put_u16_le(&frame->payload[0], acknowledged_sequence);
    frame->payload[2] = 0U;
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_ack(const nrfclaw_ninalink_frame_t *frame,
                           uint16_t *acknowledged_sequence)
{
    if (!frame || !acknowledged_sequence)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_ACK)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length != NRFCLAW_NINALINK_ACK_PAYLOAD_SIZE)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    if (frame->payload[2] != 0U)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    *acknowledged_sequence = get_u16_le(&frame->payload[0]);
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_build_nack(nrfclaw_ninalink_frame_t *frame,
                            uint16_t network_id,
                            uint32_t node_id,
                            uint16_t sequence,
                            uint16_t rejected_sequence,
                            uint8_t reason)
{
    if (!frame)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (reason < NRFCLAW_NINALINK_NACK_BAD_FRAME ||
        reason > NRFCLAW_NINALINK_NACK_INTERNAL)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    frame_init(frame,
               NRFCLAW_NINALINK_MSG_NACK,
               network_id,
               node_id,
               sequence,
               0U);

    frame->payload_length = NRFCLAW_NINALINK_ACK_PAYLOAD_SIZE;
    put_u16_le(&frame->payload[0], rejected_sequence);
    frame->payload[2] = reason;
    return NRFCLAW_NINALINK_MSG_OK;
}

nrfclaw_ninalink_msg_status_t
nrfclaw_ninalink_parse_nack(const nrfclaw_ninalink_frame_t *frame,
                            uint16_t *rejected_sequence,
                            uint8_t *reason)
{
    if (!frame || !rejected_sequence || !reason)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_ARG;

    if (frame->message_type != NRFCLAW_NINALINK_MSG_NACK)
        return NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE;

    if (frame->payload_length != NRFCLAW_NINALINK_ACK_PAYLOAD_SIZE)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH;

    if (frame->payload[2] < NRFCLAW_NINALINK_NACK_BAD_FRAME ||
        frame->payload[2] > NRFCLAW_NINALINK_NACK_INTERNAL)
        return NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE;

    *rejected_sequence = get_u16_le(&frame->payload[0]);
    *reason = frame->payload[2];
    return NRFCLAW_NINALINK_MSG_OK;
}

const char *
nrfclaw_ninalink_msg_status_str(nrfclaw_ninalink_msg_status_t status)
{
    switch (status) {
        case NRFCLAW_NINALINK_MSG_OK:
            return "OK";
        case NRFCLAW_NINALINK_MSG_ERR_BAD_ARG:
            return "BAD_ARG";
        case NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE:
            return "WRONG_TYPE";
        case NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH:
            return "BAD_LENGTH";
        case NRFCLAW_NINALINK_MSG_ERR_TOO_MANY:
            return "TOO_MANY";
        case NRFCLAW_NINALINK_MSG_ERR_NO_SPACE:
            return "NO_SPACE";
        case NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE:
            return "BAD_VALUE";
        case NRFCLAW_NINALINK_MSG_ERR_UNSUPPORTED_VALUE_TYPE:
            return "UNSUPPORTED_VALUE_TYPE";
        default:
            return "UNKNOWN";
    }
}
