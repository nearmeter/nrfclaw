#ifndef NRFCLAW_FLASH_H
#define NRFCLAW_FLASH_H

#include <stdbool.h>
#include <stdint.h>
#include "nrfclaw_auth.h"
#include "nrfclaw_schedule.h"

#define NRFCLAW_FLASH_SLOT0_ADDR        0x00070000UL
#define NRFCLAW_FLASH_SLOT1_ADDR        0x00071000UL

typedef enum
{
    NRFCLAW_FLASH_EMPTY = 0,
    NRFCLAW_FLASH_READY,
    NRFCLAW_FLASH_SAVING,
    NRFCLAW_FLASH_ERROR
} nrfclaw_flash_status_t;

void nrfclaw_flash_init(void);
void nrfclaw_flash_process(void);

bool nrfclaw_flash_request_save(uint8_t const *program,
                                uint16_t len,
                                uint16_t crc16,
                                nrfclaw_schedule_t const *schedule,
                                uint8_t const auth_tag[NRFCLAW_AUTH_TAG_SIZE]);

bool nrfclaw_flash_get_latest(uint8_t const **program,
                              uint16_t *len,
                              nrfclaw_schedule_t *schedule);

nrfclaw_flash_status_t nrfclaw_flash_status(void);
uint8_t  nrfclaw_flash_active_slot(void);     /* 0, 1 or 0xFF */
uint32_t nrfclaw_flash_generation(void);

#endif
