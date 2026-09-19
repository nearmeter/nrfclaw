#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nrfclaw_ninalink_auto_discovery.h"
#include "nrfclaw_ninalink_bridge.h"
#include "nrfclaw_ninalink_node_registry.h"

static unsigned g_tests;
static unsigned g_failures;
#define CHECK(label, cond) do { \
    g_tests++; \
    if (cond) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

static nrfclaw_ninalink_capability_discovery_status_t g_caps;
static nrfclaw_ninalink_command_discovery_status_t g_cmds;
static uint16_t g_next_caps_seq = 10U;
static uint16_t g_next_cmd_seq = 20U;
static unsigned g_caps_cancel;

bool nrfclaw_ninalink_bridge_queue_capability_discovery(
    uint32_t node, uint8_t page)
{
    if (g_caps.pending || g_cmds.pending) return false;
    memset(&g_caps, 0, sizeof(g_caps));
    g_caps.pending = true;
    g_caps.target_node = node;
    g_caps.request_seq = g_next_caps_seq++;
    g_caps.page_index = page;
    g_caps.state = NRFCLAW_NINALINK_APP_DL_PENDING;
    return true;
}
void nrfclaw_ninalink_bridge_get_capability_discovery_status(
    nrfclaw_ninalink_capability_discovery_status_t *out) { *out = g_caps; }
bool nrfclaw_ninalink_bridge_queue_command_discovery(
    uint32_t node, uint8_t start)
{
    if (g_caps.pending || g_cmds.pending) return false;
    memset(&g_cmds, 0, sizeof(g_cmds));
    g_cmds.pending = true;
    g_cmds.target_node = node;
    g_cmds.request_seq = g_next_cmd_seq++;
    g_cmds.start_index = start;
    g_cmds.state = NRFCLAW_NINALINK_APP_DL_PENDING;
    return true;
}
void nrfclaw_ninalink_bridge_get_command_discovery_status(
    nrfclaw_ninalink_command_discovery_status_t *out) { *out = g_cmds; }
bool nrfclaw_ninalink_bridge_cancel_capability_discovery(
    uint32_t node, uint16_t seq)
{
    if (!g_caps.pending || g_caps.target_node != node || g_caps.request_seq != seq)
        return false;
    g_caps.pending = false;
    g_caps.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    g_caps_cancel++;
    return true;
}
bool nrfclaw_ninalink_bridge_cancel_command_discovery(
    uint32_t node, uint16_t seq)
{
    if (!g_cmds.pending || g_cmds.target_node != node || g_cmds.request_seq != seq)
        return false;
    g_cmds.pending = false;
    g_cmds.state = NRFCLAW_NINALINK_APP_DL_IDLE;
    return true;
}

static void observe(uint32_t node)
{
    nrfclaw_ninalink_frame_t f;
    memset(&f, 0, sizeof(f));
    f.node_id = node;
    f.message_type = NRFCLAW_NINALINK_MSG_CAP_REPORT;
    nrfclaw_ninalink_node_registry_observe(&f, -40, 48, 1U);
}

static nrfclaw_ninalink_node_discovery_t getd(uint32_t node)
{
    nrfclaw_ninalink_node_discovery_t d;
    memset(&d, 0, sizeof(d));
    CHECK("discovery entry exists",
          nrfclaw_ninalink_node_registry_discovery_get(node, &d));
    return d;
}

int main(void)
{
    const uint32_t A = 0xAD64D423UL;
    const uint32_t B = 0x11112222UL;
    nrfclaw_ninalink_node_discovery_t d;
    nrfclaw_ninalink_auto_discovery_status_t as;

    nrfclaw_ninalink_node_registry_clear();
    nrfclaw_ninalink_auto_discovery_reset_runtime();
    observe(A);
    d = getd(A);
    CHECK("new node starts NEW", d.state == NRFCLAW_NINALINK_NODE_DISC_NEW);

    nrfclaw_ninalink_auto_discovery_on_contact(A);
    d = getd(A);
    CHECK("first contact starts CAPS", d.state == NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING);
    CHECK("page zero queued", g_caps.pending && g_caps.page_index == 0U);

    g_caps.pending = false;
    g_caps.state = NRFCLAW_NINALINK_APP_DL_DONE;
    g_caps.registry_version = 1U;
    g_caps.count = 5U;
    g_caps.more = true;
    nrfclaw_ninalink_auto_discovery_process();
    d = getd(A);
    CHECK("page zero accumulated", d.capability_count == 5U && d.next_capability_page == 1U);

    nrfclaw_ninalink_auto_discovery_on_contact(A);
    CHECK("page one queued", g_caps.pending && g_caps.page_index == 1U);
    g_caps.pending = false;
    g_caps.state = NRFCLAW_NINALINK_APP_DL_DONE;
    g_caps.registry_version = 1U;
    g_caps.count = 2U;
    g_caps.more = false;
    nrfclaw_ninalink_auto_discovery_process();
    d = getd(A);
    CHECK("caps complete", d.capability_count == 7U && d.state == NRFCLAW_NINALINK_NODE_DISC_CAPS_DONE);

    nrfclaw_ninalink_auto_discovery_on_contact(A);
    d = getd(A);
    CHECK("commands start", d.state == NRFCLAW_NINALINK_NODE_DISC_COMMANDS_PENDING);
    CHECK("command page zero queued", g_cmds.pending && g_cmds.start_index == 0U);

    g_cmds.pending = false;
    g_cmds.state = NRFCLAW_NINALINK_APP_DL_DONE;
    g_cmds.registry_version = 1U;
    g_cmds.total_count = 3U;
    g_cmds.count = 3U;
    nrfclaw_ninalink_auto_discovery_process();
    d = getd(A);
    CHECK("node READY", d.state == NRFCLAW_NINALINK_NODE_DISC_READY);
    CHECK("commands stored", d.command_count == 3U && d.command_registry_version == 1U);

    observe(B);
    nrfclaw_ninalink_auto_discovery_on_contact(B);
    CHECK("second node CAPS queued", g_caps.pending);
    g_caps.timeout_count = 1U;
    nrfclaw_ninalink_auto_discovery_process();
    d = getd(B);
    CHECK("timeout remains retryable", d.state == NRFCLAW_NINALINK_NODE_DISC_CAPS_PENDING);
    CHECK("timeout retry/error recorded", d.retry_count == 1U && d.last_error == NRFCLAW_NINALINK_NODE_DISC_ERR_CAPS_TIMEOUT);
    CHECK("timeout releases engine", g_caps_cancel == 1U && !g_caps.pending);

    nrfclaw_ninalink_auto_discovery_set_enabled(false);
    nrfclaw_ninalink_auto_discovery_get_status(&as);
    CHECK("can disable auto discovery", !as.enabled);
    nrfclaw_ninalink_auto_discovery_on_contact(B);
    CHECK("disabled queues nothing", !g_caps.pending);
    nrfclaw_ninalink_auto_discovery_set_enabled(true);
    nrfclaw_ninalink_auto_discovery_on_contact(B);
    CHECK("re-enabled retries", g_caps.pending && g_caps.page_index == 0U);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
