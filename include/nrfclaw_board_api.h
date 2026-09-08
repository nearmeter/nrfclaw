#ifndef NRFCLAW_BOARD_API_H
#define NRFCLAW_BOARD_API_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t board_type;
    uint8_t hw_rev_major;
    uint8_t hw_rev_minor;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    uint8_t fw_prerelease;
    uint16_t fw_build;
    uint32_t git32;
    uint8_t ndp_version;
    uint8_t vm_abi_version;
    uint8_t softdevice_family;
    uint8_t softdevice_major;
    uint8_t softdevice_minor;
    uint8_t softdevice_patch;
    uint32_t device_id0;
    uint32_t device_id1;
} nrfclaw_board_info_t;

typedef struct {
    uint32_t flash_total_bytes;
    uint32_t ram_total_bytes;
    uint32_t flash_page_size;
    uint32_t flash_page_count;
    uint32_t nrfclaw_data_start;
    uint32_t nrfclaw_data_end;
    uint32_t nrfclaw_data_free_start;
    uint32_t nrfclaw_data_free_bytes;
} nrfclaw_board_memory_t;

typedef struct {
    uint32_t resetreas;
} nrfclaw_board_reset_t;

typedef struct {
    bool present;
    uint32_t address;
} nrfclaw_board_bootloader_t;

void nrfclaw_board_api_init(void);
void nrfclaw_board_get_info(nrfclaw_board_info_t *out);
void nrfclaw_board_get_memory(nrfclaw_board_memory_t *out);
void nrfclaw_board_get_reset(nrfclaw_board_reset_t *out);
void nrfclaw_board_get_bootloader(nrfclaw_board_bootloader_t *out);

#endif
