#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nrfclaw_ninalink_command_registry.h"

static unsigned g_tests;
static unsigned g_failures;

#define CHECK(label, condition) do { \
    g_tests++; \
    if (condition) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int main(void)
{
    nrfclaw_ninalink_command_context_t ctx;
    nrfclaw_ninalink_command_descriptor_t d;
    uint8_t args[4] = {0x78U,0x56U,0x34U,0x12U};
    uint8_t result[16];
    uint8_t len;
    uint8_t st;

    memset(&ctx, 0, sizeof(ctx));
    ctx.node_id = 0xAD64D423U;

    CHECK("registry has three commands",
          nrfclaw_ninalink_command_registry_count() == 3U);

    CHECK("descriptor ECHO",
          nrfclaw_ninalink_command_registry_get(0U, &d) &&
          d.command_id == NRFCLAW_NINALINK_COMMAND_ECHO_U32 &&
          d.min_args == 4U && d.max_args == 4U && d.max_result == 4U);

    CHECK("descriptor GET_NODE_INFO",
          nrfclaw_ninalink_command_registry_get(1U, &d) &&
          d.command_id == NRFCLAW_NINALINK_COMMAND_GET_NODE_INFO &&
          d.min_args == 0U && d.max_args == 0U && d.max_result == 8U);

    CHECK("descriptor GET_TRACKING_STATE",
          nrfclaw_ninalink_command_registry_get(2U, &d) &&
          d.command_id == NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE &&
          d.min_args == 0U && d.max_args == 0U && d.max_result == 1U);

    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, NRFCLAW_NINALINK_COMMAND_ECHO_U32,
        args, 4U, result, &len);
    CHECK("ECHO status", st == NRFCLAW_NINALINK_COMMAND_STATUS_OK);
    CHECK("ECHO bytes", len == 4U && memcmp(args, result, 4U) == 0);

    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, NRFCLAW_NINALINK_COMMAND_ECHO_U32,
        args, 3U, result, &len);
    CHECK("ECHO bad args",
          st == NRFCLAW_NINALINK_COMMAND_STATUS_BAD_ARGS && len == 0U);

    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, 0x7FFFU, NULL, 0U, result, &len);
    CHECK("unsupported command",
          st == NRFCLAW_NINALINK_COMMAND_STATUS_UNSUPPORTED && len == 0U);

    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, NRFCLAW_NINALINK_COMMAND_GET_NODE_INFO,
        NULL, 0U, result, &len);
    CHECK("GET_NODE_INFO status",
          st == NRFCLAW_NINALINK_COMMAND_STATUS_OK);
    CHECK("GET_NODE_INFO layout",
          len == 8U &&
          result[0] == 1U &&
          result[1] == 64U &&
          result[2] == 3U &&
          result[3] == 0x0FU &&
          get_u32_le(&result[4]) == 0xAD64D423U);

    ctx.tracking_active = false;
    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE,
        NULL, 0U, result, &len);
    CHECK("GET_TRACKING_STATE false",
          st == NRFCLAW_NINALINK_COMMAND_STATUS_OK &&
          len == 1U && result[0] == 0U);

    ctx.tracking_active = true;
    len = 0U;
    st = nrfclaw_ninalink_command_registry_execute(
        &ctx, NRFCLAW_NINALINK_COMMAND_GET_TRACKING_STATE,
        NULL, 0U, result, &len);
    CHECK("GET_TRACKING_STATE true",
          st == NRFCLAW_NINALINK_COMMAND_STATUS_OK &&
          len == 1U && result[0] == 1U);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
