#include "nrfclaw_ninalink_msg.h"

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

static void test_hello(void)
{
    nrfclaw_ninalink_hello_t in;
    nrfclaw_ninalink_hello_t out;
    nrfclaw_ninalink_frame_t f;
    uint8_t bytes[64];
    uint8_t bytes_len = 0U;
    nrfclaw_ninalink_frame_t decoded;

    memset(&in, 0, sizeof(in));
    in.device_id0 = 0xAD64D423UL;
    in.device_id1 = 0xE3F314BBUL;
    in.capability_registry_version = 1U;
    in.advertised_capability_count = 7U;
    in.board_type = 1U;
    in.hw_rev_major = 1U;
    in.hw_rev_minor = 1U;
    in.fw_major = 10U;
    in.fw_minor = 2U;
    in.fw_patch = 3U;
    in.fw_prerelease = 1U;
    in.fw_build = 1U;

    CHECK("HELLO build",
          nrfclaw_ninalink_build_hello(&f, 0x3344U,
                                      0xAD64D423UL, 0x0102U, true, &in) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("HELLO header fields",
          f.message_type == NRFCLAW_NINALINK_MSG_HELLO &&
          f.flags == NRFCLAW_NINALINK_FLAG_ACK_REQ &&
          f.network_id == 0x3344U &&
          f.node_id == 0xAD64D423UL &&
          f.sequence == 0x0102U &&
          f.payload_length == NRFCLAW_NINALINK_HELLO_PAYLOAD_SIZE);

    CHECK("HELLO core encode",
          nrfclaw_ninalink_encode(&f, bytes, sizeof(bytes), &bytes_len) ==
          NRFCLAW_NINALINK_OK);

    CHECK("HELLO core decode",
          nrfclaw_ninalink_decode(bytes, bytes_len, &decoded) ==
          NRFCLAW_NINALINK_OK);

    memset(&out, 0, sizeof(out));
    CHECK("HELLO parse",
          nrfclaw_ninalink_parse_hello(&decoded, &out) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("HELLO roundtrip",
          memcmp(&in, &out, sizeof(in)) == 0);
}

static void fill_desc(nrfclaw_ninalink_capability_descriptor_t *d,
                      uint16_t id,
                      uint8_t channel,
                      uint8_t kind,
                      uint8_t type,
                      int8_t scale10,
                      uint8_t unit,
                      uint8_t behavior,
                      uint8_t state)
{
    memset(d, 0, sizeof(*d));
    d->desc.capability_id = id;
    d->desc.channel = channel;
    d->desc.kind = kind;
    d->desc.value_type = type;
    d->desc.scale10 = scale10;
    d->desc.unit = unit;
    d->desc.behavior_flags = behavior;
    d->runtime_state_flags = state;
}

static void test_caps_page(void)
{
    nrfclaw_ninalink_capability_descriptor_t in[5];
    nrfclaw_ninalink_capability_descriptor_t out[5];
    nrfclaw_ninalink_frame_t f;
    uint8_t registry_version = 0U;
    uint8_t page_index = 0U;
    uint8_t count = 0U;
    bool more = false;
    uint8_t bytes[64];
    uint8_t bytes_len = 0U;

    fill_desc(&in[0], NRFCLAW_SEMCAP_BATTERY_VOLTAGE, 0U,
              NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_U16, -3,
              NRFCLAW_CAP_UNIT_VOLT,
              NRFCLAW_CAP_BEHAVIOR_READABLE | NRFCLAW_CAP_BEHAVIOR_REPORTABLE,
              NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT |
              NRFCLAW_CAP_STATE_ENABLED);

    fill_desc(&in[1], NRFCLAW_SEMCAP_TEMPERATURE, 0U,
              NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S16, -2,
              NRFCLAW_CAP_UNIT_CELSIUS,
              NRFCLAW_CAP_BEHAVIOR_READABLE | NRFCLAW_CAP_BEHAVIOR_REPORTABLE,
              NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT |
              NRFCLAW_CAP_STATE_ENABLED);

    fill_desc(&in[2], NRFCLAW_SEMCAP_MOTION, 0U,
              NRFCLAW_CAP_KIND_STATE, NRFCLAW_CAP_VALUE_BOOL, 0,
              NRFCLAW_CAP_UNIT_BOOLEAN,
              NRFCLAW_CAP_BEHAVIOR_REPORTABLE |
              NRFCLAW_CAP_BEHAVIOR_EVENT_SOURCE,
              NRFCLAW_CAP_STATE_SUPPORTED);

    fill_desc(&in[3], NRFCLAW_SEMCAP_COUNTER, 0U,
              NRFCLAW_CAP_KIND_COUNTER, NRFCLAW_CAP_VALUE_U32, 0,
              NRFCLAW_CAP_UNIT_COUNT,
              NRFCLAW_CAP_BEHAVIOR_READABLE | NRFCLAW_CAP_BEHAVIOR_REPORTABLE,
              NRFCLAW_CAP_STATE_SUPPORTED);

    /* Unknown/vendor ID must be transportable without reinterpretation. */
    fill_desc(&in[4], 0x9001U, 3U,
              NRFCLAW_CAP_KIND_MEASUREMENT, NRFCLAW_CAP_VALUE_S32, -3,
              NRFCLAW_CAP_UNIT_NONE,
              NRFCLAW_CAP_BEHAVIOR_REPORTABLE,
              NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT);

    CHECK("CAPS_RESPONSE build five descriptors",
          nrfclaw_ninalink_build_caps_response(
              &f, 0U, 0x11223344UL, 9U, false, true,
              NRFCLAW_CAPABILITY_REGISTRY_VERSION, 2U, in, 5U) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("CAPS_RESPONSE payload exactly 49",
          f.payload_length == NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE);

    CHECK("CAPS_RESPONSE MORE flag",
          (f.flags & NRFCLAW_NINALINK_FLAG_MORE) != 0U);

    CHECK("CAPS_RESPONSE encodes to exactly 64-byte frame",
          nrfclaw_ninalink_encode(&f, bytes, sizeof(bytes), &bytes_len) ==
          NRFCLAW_NINALINK_OK &&
          bytes_len == NRFCLAW_NINALINK_MAX_FRAME_SIZE);

    memset(out, 0, sizeof(out));
    CHECK("CAPS_RESPONSE parse",
          nrfclaw_ninalink_parse_caps_response(
              &f, &registry_version, &page_index,
              out, 5U, &count, &more) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("CAPS_RESPONSE metadata roundtrip",
          registry_version == NRFCLAW_CAPABILITY_REGISTRY_VERSION &&
          page_index == 2U && count == 5U && more);

    CHECK("CAPS_RESPONSE descriptors roundtrip",
          memcmp(in, out, sizeof(in)) == 0);

    CHECK("CAPS_RESPONSE sixth descriptor rejected",
          nrfclaw_ninalink_build_caps_response(
              &f, 0U, 1U, 1U, false, false, 1U, 0U, in, 6U) ==
          NRFCLAW_NINALINK_MSG_ERR_TOO_MANY);
}

static void test_caps_request(void)
{
    nrfclaw_ninalink_frame_t f;
    uint8_t page = 0U;

    CHECK("CAPS_REQUEST build",
          nrfclaw_ninalink_build_caps_request(
              &f, 0x0102U, 0x1234UL, 5U, true, 7U) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("CAPS_REQUEST parse",
          nrfclaw_ninalink_parse_caps_request(&f, &page) ==
          NRFCLAW_NINALINK_MSG_OK &&
          page == 7U);
}

static void set_u16_entry(nrfclaw_ninalink_value_entry_t *e,
                          uint16_t id, uint8_t ch, uint16_t v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = ch;
    e->value.type = NRFCLAW_CAP_VALUE_U16;
    e->value.v.u16 = v;
}

static void set_s16_entry(nrfclaw_ninalink_value_entry_t *e,
                          uint16_t id, uint8_t ch, int16_t v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = ch;
    e->value.type = NRFCLAW_CAP_VALUE_S16;
    e->value.v.s16 = v;
}

static void set_u32_entry(nrfclaw_ninalink_value_entry_t *e,
                          uint16_t id, uint8_t ch, uint32_t v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = ch;
    e->value.type = NRFCLAW_CAP_VALUE_U32;
    e->value.v.u32 = v;
}

static void set_s32_entry(nrfclaw_ninalink_value_entry_t *e,
                          uint16_t id, uint8_t ch, int32_t v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = ch;
    e->value.type = NRFCLAW_CAP_VALUE_S32;
    e->value.v.s32 = v;
}

static void set_bool_entry(nrfclaw_ninalink_value_entry_t *e,
                           uint16_t id, uint8_t ch, bool v)
{
    memset(e, 0, sizeof(*e));
    e->capability_id = id;
    e->channel = ch;
    e->value.type = NRFCLAW_CAP_VALUE_BOOL;
    e->value.v.boolean = v;
}

static void test_reports(void)
{
    nrfclaw_ninalink_value_entry_t in[4];
    nrfclaw_ninalink_value_entry_t out[4];
    nrfclaw_ninalink_frame_t f;
    uint8_t count = 0U;

    set_u16_entry(&in[0], NRFCLAW_SEMCAP_BATTERY_VOLTAGE, 0U, 3300U);
    set_s16_entry(&in[1], NRFCLAW_SEMCAP_TEMPERATURE, 0U, 1988);
    set_bool_entry(&in[2], NRFCLAW_SEMCAP_TRACKING_ACTIVE, 0U, true);
    set_s32_entry(&in[3], 0x9002U, 4U, -123456);

    CHECK("CAP_REPORT build mixed values",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_CAP_REPORT,
              1U, 0xAABBCCDDUL, 100U, true, in, 4U) ==
          NRFCLAW_NINALINK_MSG_OK);

    memset(out, 0, sizeof(out));
    CHECK("CAP_REPORT parse mixed values",
          nrfclaw_ninalink_parse_values(&f, out, 4U, &count) ==
          NRFCLAW_NINALINK_MSG_OK &&
          count == 4U);

    CHECK("CAP_REPORT battery",
          out[0].capability_id == NRFCLAW_SEMCAP_BATTERY_VOLTAGE &&
          out[0].value.type == NRFCLAW_CAP_VALUE_U16 &&
          out[0].value.v.u16 == 3300U);

    CHECK("CAP_REPORT signed temperature",
          out[1].capability_id == NRFCLAW_SEMCAP_TEMPERATURE &&
          out[1].value.v.s16 == 1988);

    CHECK("CAP_REPORT bool",
          out[2].capability_id == NRFCLAW_SEMCAP_TRACKING_ACTIVE &&
          out[2].value.v.boolean);

    CHECK("CAP_REPORT unknown capability preserved",
          out[3].capability_id == 0x9002U &&
          out[3].channel == 4U &&
          out[3].value.type == NRFCLAW_CAP_VALUE_S32 &&
          out[3].value.v.s32 == -123456);
}

static void test_max_values(void)
{
    nrfclaw_ninalink_value_entry_t in[7];
    nrfclaw_ninalink_frame_t f;
    uint8_t i;
    uint8_t bytes[64];
    uint8_t bytes_len = 0U;

    for (i = 0U; i < 7U; i++)
        set_u32_entry(&in[i], (uint16_t)(0x9000U + i), i, i * 1000UL);

    CHECK("six U32 entries fill 49-byte payload",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_CAP_REPORT,
              0U, 1U, 1U, false, in, 6U) ==
          NRFCLAW_NINALINK_MSG_OK &&
          f.payload_length == NRFCLAW_NINALINK_MAX_PAYLOAD_SIZE);

    CHECK("six U32 entries form 64-byte NinaLink frame",
          nrfclaw_ninalink_encode(&f, bytes, sizeof(bytes), &bytes_len) ==
          NRFCLAW_NINALINK_OK &&
          bytes_len == NRFCLAW_NINALINK_MAX_FRAME_SIZE);

    CHECK("seventh U32 entry rejected for payload space",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_CAP_REPORT,
              0U, 1U, 1U, false, in, 7U) ==
          NRFCLAW_NINALINK_MSG_ERR_NO_SPACE);
}

static void test_events_and_bad_values(void)
{
    nrfclaw_ninalink_value_entry_t e[2];
    nrfclaw_ninalink_value_entry_t out[2];
    nrfclaw_ninalink_frame_t f;
    uint8_t count = 0U;

    memset(e, 0, sizeof(e));
    e[0].capability_id = NRFCLAW_SEMCAP_TAP;
    e[0].value.type = NRFCLAW_CAP_VALUE_ENUM8;
    e[0].value.v.u8 = 2U;
    set_bool_entry(&e[1], NRFCLAW_SEMCAP_FALL, 0U, true);

    CHECK("CAP_EVENT build",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_CAP_EVENT,
              0U, 3U, 44U, true, e, 2U) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("CAP_EVENT parse",
          nrfclaw_ninalink_parse_values(&f, out, 2U, &count) ==
          NRFCLAW_NINALINK_MSG_OK &&
          count == 2U &&
          out[0].value.v.u8 == 2U &&
          out[1].value.v.boolean);

    /* Corrupt BOOL value in the payload to 2. */
    f.payload[f.payload_length - 1U] = 2U;
    CHECK("invalid BOOL rejected",
          nrfclaw_ninalink_parse_values(&f, out, 2U, &count) ==
          NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE);

    /* Unknown value type prevents deterministic parsing. */
    f.payload[f.payload_length - 1U] = 1U;
    f.payload[4] = 0x7FU;
    CHECK("unknown value type rejected",
          nrfclaw_ninalink_parse_values(&f, out, 2U, &count) ==
          NRFCLAW_NINALINK_MSG_ERR_UNSUPPORTED_VALUE_TYPE);
}

static void test_ack_nack(void)
{
    nrfclaw_ninalink_frame_t f;
    uint16_t seq = 0U;
    uint8_t reason = 0U;

    CHECK("ACK build",
          nrfclaw_ninalink_build_ack(
              &f, 0x1001U, 0xABCDEF01UL, 10U, 9U) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("ACK parse",
          nrfclaw_ninalink_parse_ack(&f, &seq) ==
          NRFCLAW_NINALINK_MSG_OK &&
          seq == 9U);

    CHECK("NACK build",
          nrfclaw_ninalink_build_nack(
              &f, 0x1001U, 0xABCDEF01UL, 11U, 9U,
              NRFCLAW_NINALINK_NACK_BUSY) ==
          NRFCLAW_NINALINK_MSG_OK);

    CHECK("NACK parse",
          nrfclaw_ninalink_parse_nack(&f, &seq, &reason) ==
          NRFCLAW_NINALINK_MSG_OK &&
          seq == 9U &&
          reason == NRFCLAW_NINALINK_NACK_BUSY);

    CHECK("invalid NACK reason rejected",
          nrfclaw_ninalink_build_nack(
              &f, 0U, 0U, 0U, 0U, 0x7FU) ==
          NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE);
}

static void test_malformed(void)
{
    nrfclaw_ninalink_frame_t f;
    nrfclaw_ninalink_value_entry_t entry;
    nrfclaw_ninalink_value_entry_t out;
    uint8_t count = 0U;
    uint8_t rv = 0U;
    uint8_t pi = 0U;
    bool more = false;
    nrfclaw_ninalink_capability_descriptor_t d;

    set_u16_entry(&entry, NRFCLAW_SEMCAP_BATTERY_VOLTAGE, 0U, 3300U);

    CHECK("report wrong helper type rejected",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_HELLO,
              0U, 0U, 0U, false, &entry, 1U) ==
          NRFCLAW_NINALINK_MSG_ERR_WRONG_TYPE);

    CHECK("valid one-entry report",
          nrfclaw_ninalink_build_values(
              &f, NRFCLAW_NINALINK_MSG_CAP_REPORT,
              0U, 0U, 0U, false, &entry, 1U) ==
          NRFCLAW_NINALINK_MSG_OK);

    f.payload_length--;
    CHECK("truncated value entry rejected",
          nrfclaw_ninalink_parse_values(&f, &out, 1U, &count) ==
          NRFCLAW_NINALINK_MSG_ERR_BAD_LENGTH);

    memset(&f, 0, sizeof(f));
    f.message_type = NRFCLAW_NINALINK_MSG_CAPS_RESPONSE;
    f.payload_length = 4U;
    f.payload[0] = 1U;
    f.payload[1] = 0U;
    f.payload[2] = 0U;
    f.payload[3] = 1U; /* reserved must be zero */

    CHECK("CAPS_RESPONSE reserved byte rejected",
          nrfclaw_ninalink_parse_caps_response(
              &f, &rv, &pi, &d, 1U, &count, &more) ==
          NRFCLAW_NINALINK_MSG_ERR_BAD_VALUE);
}

int main(void)
{
    test_hello();
    test_caps_page();
    test_caps_request();
    test_reports();
    test_max_values();
    test_events_and_bad_values();
    test_ack_nack();
    test_malformed();

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failed);
    if (g_failed == 0U) {
        printf("B3.2 NinaLink semantic messages: PASS\n");
        return 0;
    }

    printf("B3.2 NinaLink semantic messages: FAIL\n");
    return 1;
}
