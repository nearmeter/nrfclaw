#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nrfclaw_ninalink_capability_discovery.h"

static unsigned g_tests;
static unsigned g_failures;

#define CHECK(label, condition) do { \
    g_tests++; \
    if (condition) printf("PASS  %s\n", label); \
    else { printf("FAIL  %s\n", label); g_failures++; } \
} while (0)

typedef struct {
    nrfclaw_capability_desc_t desc;
    uint8_t state;
} stub_cap_t;

static const stub_cap_t g_caps[] = {
    { {0x0001U,0U,1U,4U,-3,3U,0x13U}, 0x07U },
    { {0x0100U,0U,1U,5U,-2,7U,0x13U}, 0x07U },
    { {0x0101U,0U,1U,4U,-2,2U,0x13U}, 0x00U },
    { {0x0200U,0U,2U,1U, 0,1U,0x16U}, 0x03U },
    { {0x0401U,0U,6U,1U, 0,1U,0x1BU}, 0x03U },
    { {0x0302U,0U,4U,6U, 0,0x14U,0x13U}, 0x07U },
    { {0x0303U,0U,5U,7U, 0,0x14U,0x13U}, 0x03U },
    { {0x0500U,0U,1U,7U,-3,3U,0x13U}, 0x00U },
};

uint16_t nrfclaw_capability_registry_count(void)
{
    return (uint16_t)(sizeof(g_caps) / sizeof(g_caps[0]));
}

bool nrfclaw_capability_registry_at(uint16_t index, nrfclaw_capability_desc_t *out)
{
    if (!out || index >= nrfclaw_capability_registry_count())
        return false;
    *out = g_caps[index].desc;
    return true;
}

bool nrfclaw_capability_state(uint16_t capability_id, uint8_t channel, uint8_t *state_flags)
{
    uint16_t i;
    if (!state_flags || channel != 0U)
        return false;
    for (i = 0U; i < nrfclaw_capability_registry_count(); i++) {
        if (g_caps[i].desc.capability_id == capability_id) {
            *state_flags = g_caps[i].state;
            return true;
        }
    }
    return false;
}

bool nrfclaw_capability_descriptor(uint16_t capability_id, uint8_t channel, nrfclaw_capability_desc_t *out)
{
    uint16_t i;
    if (!out || channel != 0U)
        return false;
    for (i = 0U; i < nrfclaw_capability_registry_count(); i++) {
        if (g_caps[i].desc.capability_id == capability_id) {
            *out = g_caps[i].desc;
            return true;
        }
    }
    return false;
}

bool nrfclaw_capability_read_current(uint16_t capability_id, uint8_t channel, nrfclaw_capability_value_t *out)
{
    (void)capability_id; (void)channel; (void)out;
    return false;
}

uint8_t nrfclaw_capability_value_size(uint8_t value_type)
{
    (void)value_type;
    return 0U;
}

int main(void)
{
    nrfclaw_ninalink_capability_discovery_page_t page;

    CHECK("supported count filters unsupported entries",
          nrfclaw_ninalink_capability_supported_count() == 6U);
    CHECK("page 0 builds",
          nrfclaw_ninalink_capability_discovery_build_page(0U, &page));
    CHECK("page 0 has five descriptors and MORE",
          page.page_index == 0U && page.count == 5U && page.more);
    CHECK("page 0 starts with battery",
          page.descriptors[0].desc.capability_id == 0x0001U);
    CHECK("page 0 preserves runtime state",
          page.descriptors[0].runtime_state_flags == 0x07U);
    CHECK("tracking descriptor present on page 0",
          page.descriptors[3].desc.capability_id == 0x0401U);
    CHECK("tracking descriptor preserves WRITABLE",
          (page.descriptors[3].desc.behavior_flags &
           NRFCLAW_CAP_BEHAVIOR_WRITABLE) != 0U);
    CHECK("tracking state supported/present not enabled",
          page.descriptors[3].runtime_state_flags ==
          (NRFCLAW_CAP_STATE_SUPPORTED | NRFCLAW_CAP_STATE_PRESENT));

    CHECK("page 1 builds",
          nrfclaw_ninalink_capability_discovery_build_page(1U, &page));
    CHECK("page 1 one descriptor no MORE",
          page.page_index == 1U && page.count == 1U && !page.more);
    CHECK("page 1 descriptor quadrature",
          page.descriptors[0].desc.capability_id == 0x0303U);

    CHECK("page 2 builds empty",
          nrfclaw_ninalink_capability_discovery_build_page(2U, &page));
    CHECK("page 2 empty no MORE", page.count == 0U && !page.more);

    printf("\n%u test(s), %u failure(s)\n", g_tests, g_failures);
    return g_failures ? 1 : 0;
}
