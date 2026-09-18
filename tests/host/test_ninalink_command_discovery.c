#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nrfclaw_ninalink_command_discovery.h"

static unsigned g_tests;
static unsigned g_failures;

#define CHECK(label, condition) do { \
    g_tests++; \
    if (condition) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

int main(void)
{
    uint8_t payload[49];
    uint8_t len = 0U;
    nrfclaw_ninalink_command_discovery_page_t page;

    CHECK("page 0 builds",
          nrfclaw_ninalink_command_discovery_build_page(
              0U, payload, sizeof(payload), &len));
    CHECK("page 0 payload length",
          len == 22U);
    CHECK("page 0 metadata",
          payload[0] == 1U &&
          payload[1] == 0U &&
          payload[2] == 3U &&
          payload[3] == 3U);
    CHECK("page 0 parses",
          nrfclaw_ninalink_command_discovery_parse_page(
              payload, len, &page));
    CHECK("page 0 descriptor count",
          page.registry_version == 1U &&
          page.start_index == 0U &&
          page.total_count == 3U &&
          page.count == 3U);
    CHECK("page 0 ECHO contract",
          page.descriptors[0].command_id ==
              NRFCLAW_NINALINK_COMMAND_ECHO_U32 &&
          page.descriptors[0].min_args == 4U &&
          page.descriptors[0].max_args == 4U &&
          page.descriptors[0].max_result == 4U &&
          page.descriptors[0].flags ==
              NRFCLAW_NINALINK_COMMAND_FLAG_READ_ONLY);
    CHECK("page 0 GET_NODE_INFO contract",
          page.descriptors[1].command_id ==
              NRFCLAW_NINALINK_COMMAND_GET_NODE_INFO &&
          page.descriptors[1].min_args == 0U &&
          page.descriptors[1].max_args == 0U &&
          page.descriptors[1].max_result == 8U);
    CHECK("page 0 GET_TRACKING_STATE contract",
          page.descriptors[2].command_id ==
              NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE &&
          page.descriptors[2].min_args == 0U &&
          page.descriptors[2].max_args == 0U &&
          page.descriptors[2].max_result == 1U);

    len = 0U;
    CHECK("page start=2 builds",
          nrfclaw_ninalink_command_discovery_build_page(
              2U, payload, sizeof(payload), &len));
    CHECK("page start=2 length",
          len == 10U);
    CHECK("page start=2 parses",
          nrfclaw_ninalink_command_discovery_parse_page(
              payload, len, &page));
    CHECK("page start=2 contains one descriptor",
          page.start_index == 2U &&
          page.total_count == 3U &&
          page.count == 1U &&
          page.descriptors[0].command_id ==
              NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE);

    len = 0U;
    CHECK("empty page at end builds",
          nrfclaw_ninalink_command_discovery_build_page(
              3U, payload, sizeof(payload), &len));
    CHECK("empty page length",
          len == 4U);
    CHECK("empty page parses",
          nrfclaw_ninalink_command_discovery_parse_page(
              payload, len, &page));
    CHECK("empty page metadata",
          page.start_index == 3U &&
          page.total_count == 3U &&
          page.count == 0U);

    len = 0U;
    CHECK("small output buffer rejected",
          !nrfclaw_ninalink_command_discovery_build_page(
              0U, payload, 21U, &len));

    payload[0] = 2U;
    CHECK("unsupported registry version rejected",
          !nrfclaw_ninalink_command_discovery_parse_page(
              payload, 4U, &page));

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
