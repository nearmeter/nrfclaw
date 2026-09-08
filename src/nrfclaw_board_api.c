#include "nrfclaw_board_api.h"
#include "nrfclaw_board.h"
#include "nrfclaw_version.h"
#include "nrf.h"

#include <string.h>

/* Persistent layout frozen in Stage 8.3. */
#define NRFCLAW_DATA_START       0x0006C000UL
#define NRFCLAW_DATA_END         0x0007A000UL
/* 0x6C000/0x6D000: optional NDP owner-key A/B store.
 * 0x6E000/0x6F000: persistent LoRa profile A/B store.
 * 0x70000..0x79000: VM/state/tracking/vibration stores.
 * 0x7A000..0x7FFFF: BLE firmware bootloader reservation.
 * Report only the highest contiguous free region. */
#define NRFCLAW_DATA_FREE_START  0x0007A000UL

static uint32_t m_boot_resetreas;

void nrfclaw_board_api_init(void)
{
    /* Capture before any later code has a chance to clear RESETREAS. */
    m_boot_resetreas = NRF_POWER->RESETREAS;
}

void nrfclaw_board_get_info(nrfclaw_board_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->board_type = NRFCLAW_BOARD_TYPE;
    out->hw_rev_major = NRFCLAW_HW_REV_MAJOR;
    out->hw_rev_minor = NRFCLAW_HW_REV_MINOR;
    out->fw_major = NRFCLAW_FW_VERSION_MAJOR;
    out->fw_minor = NRFCLAW_FW_VERSION_MINOR;
    out->fw_patch = NRFCLAW_FW_VERSION_PATCH;
    out->fw_prerelease = NRFCLAW_FW_PRERELEASE;
    out->fw_build = (uint16_t)NRFCLAW_FW_BUILD;
    out->git32 = NRFCLAW_FW_GIT32;
    out->ndp_version = NRFCLAW_NDP_VERSION;
    out->vm_abi_version = NRFCLAW_VM_ABI_VERSION;
    out->softdevice_family = NRFCLAW_SD_FAMILY;
    out->softdevice_major = NRFCLAW_SD_VERSION_MAJOR;
    out->softdevice_minor = NRFCLAW_SD_VERSION_MINOR;
    out->softdevice_patch = NRFCLAW_SD_VERSION_PATCH;
    out->device_id0 = NRF_FICR->DEVICEID[0];
    out->device_id1 = NRF_FICR->DEVICEID[1];
}

void nrfclaw_board_get_memory(nrfclaw_board_memory_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* These FICR values describe the actual fitted MCU. */
    out->flash_page_size = NRF_FICR->CODEPAGESIZE;
    out->flash_page_count = NRF_FICR->CODESIZE;
    out->flash_total_bytes = out->flash_page_size * out->flash_page_count;

#if defined(FICR_INFO_RAM_RAM_Msk)
    /* INFO.RAM is encoded in kB on nRF52. */
    out->ram_total_bytes = NRF_FICR->INFO.RAM * 1024UL;
#else
    /* nRF52832 fallback.  Kept compile-safe for older SDK headers. */
    out->ram_total_bytes = 64UL * 1024UL;
#endif

    out->nrfclaw_data_start = NRFCLAW_DATA_START;
    out->nrfclaw_data_end = NRFCLAW_DATA_END;
    out->nrfclaw_data_free_start = NRFCLAW_DATA_FREE_START;
    out->nrfclaw_data_free_bytes = NRFCLAW_DATA_END - NRFCLAW_DATA_FREE_START;
}

void nrfclaw_board_get_reset(nrfclaw_board_reset_t *out)
{
    if (!out) return;
    out->resetreas = m_boot_resetreas;
}

void nrfclaw_board_get_bootloader(nrfclaw_board_bootloader_t *out)
{
    if (!out) return;

    /* Nordic MBR bootloader start address is stored in UICR.NRFFW[0]. */
    uint32_t address = NRF_UICR->NRFFW[0];
    out->address = address;
    out->present = (address != 0xFFFFFFFFUL && address < NRF_FICR->CODEPAGESIZE * NRF_FICR->CODESIZE);
}
