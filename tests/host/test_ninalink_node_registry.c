#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nrfclaw_ninalink_node_registry.h"

static unsigned g_tests;
static unsigned g_failures;

#define CHECK(label, cond) do { \
    g_tests++; \
    if (cond) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

static nrfclaw_ninalink_frame_t mkframe(
    uint32_t node, uint16_t net, uint16_t seq, uint8_t type)
{
    nrfclaw_ninalink_frame_t f;
    memset(&f, 0, sizeof(f));
    f.node_id = node;
    f.network_id = net;
    f.sequence = seq;
    f.message_type = type;
    return f;
}

int main(void)
{
    nrfclaw_ninalink_node_registry_status_t st;
    nrfclaw_ninalink_node_entry_t e;
    nrfclaw_ninalink_frame_t f;
    uint8_t i;

    nrfclaw_ninalink_node_registry_clear();
    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("empty count", st.count == 0U);
    CHECK("capacity eight", st.capacity == 8U);

    f = mkframe(0xAD64D423UL, 0x1234U, 1U, 0x10U);
    nrfclaw_ninalink_node_registry_observe(&f, -46, 47, 100U);
    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("first node created", st.count == 1U && st.creations == 1U);
    CHECK("one update", st.updates == 1U);
    CHECK("get first", nrfclaw_ninalink_node_registry_get_at(0U, &e));
    CHECK("first identity", e.node_id == 0xAD64D423UL);
    CHECK("first metadata",
          e.network_id == 0x1234U &&
          e.last_sequence == 1U &&
          e.last_message_type == 0x10U &&
          e.seen_count == 1U &&
          e.last_seen_s == 100U &&
          e.rssi_x2 == -46 &&
          e.snr_x4 == 47);

    f.sequence = 2U;
    f.message_type = 0x11U;
    nrfclaw_ninalink_node_registry_observe(&f, -50, 40, 105U);
    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("same node not duplicated", st.count == 1U && st.creations == 1U);
    CHECK("same node updated twice", st.updates == 2U);
    CHECK("find same node",
          nrfclaw_ninalink_node_registry_find(0xAD64D423UL, &e));
    CHECK("same node latest state",
          e.last_sequence == 2U &&
          e.last_message_type == 0x11U &&
          e.seen_count == 2U &&
          e.last_seen_s == 105U &&
          e.rssi_x2 == -50 &&
          e.snr_x4 == 40);

    f = mkframe(0U, 1U, 1U, 0x10U);
    nrfclaw_ninalink_node_registry_observe(&f, 0, 0, 106U);
    f = mkframe(NRFCLAW_NINALINK_NODE_BROADCAST, 1U, 1U, 0x10U);
    nrfclaw_ninalink_node_registry_observe(&f, 0, 0, 107U);
    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("zero/broadcast ids ignored", st.count == 1U && st.updates == 2U);

    for (i = 0U; i < 7U; i++) {
        f = mkframe(0x1000UL + i, 0x0001U, i, 0x10U);
        nrfclaw_ninalink_node_registry_observe(
            &f, (int16_t)(-60 - i), (int16_t)(20 + i), 200U + i);
    }

    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("registry full", st.count == 8U);
    CHECK("eight creations", st.creations == 8U);
    CHECK("no eviction yet", st.evictions == 0U);

    f = mkframe(0xDEADBEEFUL, 0x0002U, 9U, 0x10U);
    nrfclaw_ninalink_node_registry_observe(&f, -70, 10, 300U);

    nrfclaw_ninalink_node_registry_get_status(&st);
    CHECK("capacity remains eight", st.count == 8U);
    CHECK("ninth creation counted", st.creations == 9U);
    CHECK("LRU eviction counted", st.evictions == 1U);
    CHECK("oldest node evicted",
          !nrfclaw_ninalink_node_registry_find(0xAD64D423UL, &e));
    CHECK("new node present",
          nrfclaw_ninalink_node_registry_find(0xDEADBEEFUL, &e));
    CHECK("new node link preserved", e.rssi_x2 == -70 && e.snr_x4 == 10);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
