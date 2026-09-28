#include "nrfclaw_ninalink.h"

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

uint16_t nrfclaw_ninalink_crc16(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;
    uint8_t bit;

    if (!data && length != 0U)
        return 0U;

    for (i = 0U; i < length; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U)
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            else
                crc = (uint16_t)(crc << 1);
        }
    }

    return crc;
}

nrfclaw_ninalink_status_t
nrfclaw_ninalink_encode(const nrfclaw_ninalink_frame_t *frame,
                        uint8_t *out,
                        uint8_t out_capacity,
                        uint8_t *out_length)
{
    uint8_t total_length;
    uint16_t crc;

    if (!frame || !out || !out_length)
        return NRFCLAW_NINALINK_ERR_BAD_ARG;

    *out_length = 0U;

    if (frame->payload_length > NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE)
        return NRFCLAW_NINALINK_ERR_TOO_LONG;

    if ((frame->flags & (uint8_t)~NRFCLAW_NINALINK_V1_FLAGS_MASK) != 0U)
        return NRFCLAW_NINALINK_ERR_BAD_FLAGS;

    total_length = (uint8_t)(NRFCLAW_NINALINK_HEADER_SIZE +
                             frame->payload_length +
                             NRFCLAW_NINALINK_CRC_SIZE);

    if (out_capacity < total_length)
        return NRFCLAW_NINALINK_ERR_NO_SPACE;

    out[0] = NRFCLAW_NINALINK_MAGIC;
    out[1] = NRFCLAW_NINALINK_VERSION;
    out[2] = frame->flags;
    out[3] = frame->message_type;
    put_u16_le(&out[4], frame->network_id);
    out[6] = frame->payload_length;
    put_u32_le(&out[7], frame->node_id);
    put_u16_le(&out[11], frame->sequence);

    if (frame->payload_length != 0U)
        memcpy(&out[13], frame->payload, frame->payload_length);

    crc = nrfclaw_ninalink_crc16(
        out,
        (uint16_t)(NRFCLAW_NINALINK_HEADER_SIZE + frame->payload_length));

    put_u16_le(&out[13U + frame->payload_length], crc);
    *out_length = total_length;

    return NRFCLAW_NINALINK_OK;
}

nrfclaw_ninalink_status_t
nrfclaw_ninalink_decode(const uint8_t *data,
                        uint8_t length,
                        nrfclaw_ninalink_frame_t *out)
{
    uint8_t payload_length;
    uint8_t expected_length;
    uint16_t expected_crc;
    uint16_t received_crc;

    if (!data || !out)
        return NRFCLAW_NINALINK_ERR_BAD_ARG;

    if (length < NRFCLAW_NINALINK_MIN_FRAME_SIZE)
        return NRFCLAW_NINALINK_ERR_TOO_SHORT;

    if (length > NRFCLAW_NINALINK_MAX_FRAME_SIZE)
        return NRFCLAW_NINALINK_ERR_TOO_LONG;

    if (data[0] != NRFCLAW_NINALINK_MAGIC)
        return NRFCLAW_NINALINK_ERR_BAD_MAGIC;

    if (data[1] != NRFCLAW_NINALINK_VERSION)
        return NRFCLAW_NINALINK_ERR_BAD_VERSION;

    if ((data[2] & (uint8_t)~NRFCLAW_NINALINK_V1_FLAGS_MASK) != 0U)
        return NRFCLAW_NINALINK_ERR_BAD_FLAGS;

    payload_length = data[6];
    if (payload_length > NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE)
        return NRFCLAW_NINALINK_ERR_TOO_LONG;

    expected_length = (uint8_t)(NRFCLAW_NINALINK_HEADER_SIZE +
                                payload_length +
                                NRFCLAW_NINALINK_CRC_SIZE);
    if (length != expected_length)
        return NRFCLAW_NINALINK_ERR_BAD_LENGTH;

    expected_crc = nrfclaw_ninalink_crc16(
        data,
        (uint16_t)(NRFCLAW_NINALINK_HEADER_SIZE + payload_length));

    received_crc = get_u16_le(&data[13U + payload_length]);

    if (expected_crc != received_crc)
        return NRFCLAW_NINALINK_ERR_BAD_CRC;

    memset(out, 0, sizeof(*out));
    out->flags = data[2];
    out->message_type = data[3];
    out->network_id = get_u16_le(&data[4]);
    out->node_id = get_u32_le(&data[7]);
    out->sequence = get_u16_le(&data[11]);
    out->payload_length = payload_length;

    if (payload_length != 0U)
        memcpy(out->payload, &data[13], payload_length);

    return NRFCLAW_NINALINK_OK;
}

const char *nrfclaw_ninalink_status_str(nrfclaw_ninalink_status_t status)
{
    switch (status) {
        case NRFCLAW_NINALINK_OK:
            return "OK";
        case NRFCLAW_NINALINK_ERR_BAD_ARG:
            return "BAD_ARG";
        case NRFCLAW_NINALINK_ERR_NO_SPACE:
            return "NO_SPACE";
        case NRFCLAW_NINALINK_ERR_TOO_SHORT:
            return "TOO_SHORT";
        case NRFCLAW_NINALINK_ERR_TOO_LONG:
            return "TOO_LONG";
        case NRFCLAW_NINALINK_ERR_BAD_MAGIC:
            return "BAD_MAGIC";
        case NRFCLAW_NINALINK_ERR_BAD_VERSION:
            return "BAD_VERSION";
        case NRFCLAW_NINALINK_ERR_BAD_FLAGS:
            return "BAD_FLAGS";
        case NRFCLAW_NINALINK_ERR_BAD_LENGTH:
            return "BAD_LENGTH";
        case NRFCLAW_NINALINK_ERR_BAD_CRC:
            return "BAD_CRC";
        default:
            return "UNKNOWN";
    }
}
