#include "nrfclaw_ninalink.h"

#include <stdio.h>
#include <string.h>

static unsigned g_tests = 0U;
static unsigned g_failed = 0U;

#define CHECK(name, cond)                                                     \
    do {                                                                      \
        g_tests++;                                                            \
        if (cond)                                                             \
            printf("PASS  %s\n", name);                                       \
        else {                                                                \
            printf("FAIL  %s\n", name);                                       \
            g_failed++;                                                       \
        }                                                                     \
    } while (0)

static void test_crc_standard_vector(void)
{
    static const uint8_t data[] = "123456789";
    CHECK("CRC16/CCITT-FALSE standard vector",
          nrfclaw_ninalink_crc16(data, 9U) == 0x29B1U);
}

static void test_golden_vector(void)
{
    nrfclaw_ninalink_frame_t f;
    nrfclaw_ninalink_frame_t d;
    uint8_t out[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t out_len = 0U;
    nrfclaw_ninalink_status_t st;

    static const uint8_t expected[] = {
        0x4E, 0x01, 0x01, 0x10,
        0x34, 0x12, 0x03,
        0x78, 0x56, 0x34, 0x12,
        0xCD, 0xAB,
        0xAA, 0x55, 0x01,
        0xC9, 0x59
    };

    memset(&f, 0, sizeof(f));
    f.flags = NRFCLAW_NINALINK_FLAG_ACK_REQ;
    f.message_type = NRFCLAW_NINALINK_MSG_CAP_REPORT;
    f.network_id = 0x1234U;
    f.node_id = 0x12345678UL;
    f.sequence = 0xABCDU;
    f.payload_length = 3U;
    f.payload[0] = 0xAAU;
    f.payload[1] = 0x55U;
    f.payload[2] = 0x01U;

    st = nrfclaw_ninalink_encode(&f, out, sizeof(out), &out_len);
    CHECK("golden encode status", st == NRFCLAW_NINALINK_OK);
    CHECK("golden encode length", out_len == sizeof(expected));
    CHECK("golden encode bytes",
          out_len == sizeof(expected) &&
          memcmp(out, expected, sizeof(expected)) == 0);

    memset(&d, 0, sizeof(d));
    st = nrfclaw_ninalink_decode(expected, sizeof(expected), &d);
    CHECK("golden decode status", st == NRFCLAW_NINALINK_OK);
    CHECK("golden decode fields",
          d.flags == NRFCLAW_NINALINK_FLAG_ACK_REQ &&
          d.message_type == NRFCLAW_NINALINK_MSG_CAP_REPORT &&
          d.network_id == 0x1234U &&
          d.node_id == 0x12345678UL &&
          d.sequence == 0xABCDU &&
          d.payload_length == 3U &&
          d.payload[0] == 0xAAU &&
          d.payload[1] == 0x55U &&
          d.payload[2] == 0x01U);
}

static void test_maximum_frame(void)
{
    nrfclaw_ninalink_frame_t f;
    nrfclaw_ninalink_frame_t d;
    uint8_t out[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t out_len = 0U;
    uint8_t i;

    memset(&f, 0, sizeof(f));
    f.flags = NRFCLAW_NINALINK_FLAG_MORE;
    f.message_type = 0x7EU; /* Unknown message type must remain transportable. */
    f.network_id = 0xBEEFU;
    f.node_id = 0x01020304UL;
    f.sequence = 0xFFFFU;
    f.payload_length = NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE;

    for (i = 0U; i < f.payload_length; i++)
        f.payload[i] = i;

    CHECK("max frame encode",
          nrfclaw_ninalink_encode(&f, out, sizeof(out), &out_len) ==
          NRFCLAW_NINALINK_OK);
    CHECK("max frame is exactly 64 bytes",
          out_len == NRFCLAW_NINALINK_MAX_FRAME_SIZE);
    CHECK("unknown message type decodes",
          nrfclaw_ninalink_decode(out, out_len, &d) ==
          NRFCLAW_NINALINK_OK);
    CHECK("max frame payload roundtrip",
          d.message_type == 0x7EU &&
          d.payload_length == NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE &&
          memcmp(d.payload, f.payload, f.payload_length) == 0);
}

static void test_errors(void)
{
    nrfclaw_ninalink_frame_t f;
    nrfclaw_ninalink_frame_t d;
    uint8_t out[NRFCLAW_NINALINK_MAX_FRAME_SIZE];
    uint8_t work[NRFCLAW_NINALINK_MAX_FRAME_SIZE + 1U];
    uint8_t out_len = 0U;

    memset(&f, 0, sizeof(f));
    f.message_type = NRFCLAW_NINALINK_MSG_HELLO;
    f.network_id = 0U;
    f.node_id = 0xAABBCCDDUL;
    f.sequence = 1U;
    f.payload_length = 1U;
    f.payload[0] = 0x42U;

    CHECK("base frame encode",
          nrfclaw_ninalink_encode(&f, out, sizeof(out), &out_len) ==
          NRFCLAW_NINALINK_OK);

    memcpy(work, out, out_len);
    work[13] ^= 0x01U;
    CHECK("bad CRC rejected",
          nrfclaw_ninalink_decode(work, out_len, &d) ==
          NRFCLAW_NINALINK_ERR_BAD_CRC);

    CHECK("truncated frame rejected",
          nrfclaw_ninalink_decode(out, (uint8_t)(out_len - 1U), &d) ==
          NRFCLAW_NINALINK_ERR_BAD_LENGTH);

    memcpy(work, out, out_len);
    work[6] = 2U;
    CHECK("declared payload length mismatch rejected",
          nrfclaw_ninalink_decode(work, out_len, &d) ==
          NRFCLAW_NINALINK_ERR_BAD_LENGTH);

    memcpy(work, out, out_len);
    work[0] = 0x00U;
    CHECK("bad magic rejected",
          nrfclaw_ninalink_decode(work, out_len, &d) ==
          NRFCLAW_NINALINK_ERR_BAD_MAGIC);

    memcpy(work, out, out_len);
    work[1] = 0x02U;
    CHECK("bad version rejected",
          nrfclaw_ninalink_decode(work, out_len, &d) ==
          NRFCLAW_NINALINK_ERR_BAD_VERSION);

    memcpy(work, out, out_len);
    work[2] = NRFCLAW_NINALINK_FLAG_AUTH;
    CHECK("unsupported AUTH flag rejected",
          nrfclaw_ninalink_decode(work, out_len, &d) ==
          NRFCLAW_NINALINK_ERR_BAD_FLAGS);

    f.flags = NRFCLAW_NINALINK_FLAG_ENCRYPTED;
    CHECK("unsupported ENCRYPTED flag rejected on encode",
          nrfclaw_ninalink_encode(&f, out, sizeof(out), &out_len) ==
          NRFCLAW_NINALINK_ERR_BAD_FLAGS);

    f.flags = 0U;
    f.payload_length = (uint8_t)(NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE + 1U);
    CHECK("oversize payload rejected on encode",
          nrfclaw_ninalink_encode(&f, out, sizeof(out), &out_len) ==
          NRFCLAW_NINALINK_ERR_TOO_LONG);

    f.payload_length = 1U;
    CHECK("insufficient output buffer rejected",
          nrfclaw_ninalink_encode(&f, out,
                                  NRFCLAW_NINALINK_MIN_FRAME_SIZE,
                                  &out_len) ==
          NRFCLAW_NINALINK_ERR_NO_SPACE);

    memset(work, 0, sizeof(work));
    CHECK("physical frame >64 rejected",
          nrfclaw_ninalink_decode(work,
                                  (uint8_t)(NRFCLAW_NINALINK_MAX_FRAME_SIZE + 1U),
                                  &d) ==
          NRFCLAW_NINALINK_ERR_TOO_LONG);

    CHECK("physical frame <15 rejected",
          nrfclaw_ninalink_decode(out,
                                  (uint8_t)(NRFCLAW_NINALINK_MIN_FRAME_SIZE - 1U),
                                  &d) ==
          NRFCLAW_NINALINK_ERR_TOO_SHORT);
}

int main(void)
{
    test_crc_standard_vector();
    test_golden_vector();
    test_maximum_frame();
    test_errors();

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failed);
    if (g_failed == 0U) {
        printf("B3.1 NinaLink codec: PASS\n");
        return 0;
    }

    printf("B3.1 NinaLink codec: FAIL\n");
    return 1;
}
