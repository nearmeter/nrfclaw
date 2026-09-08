#ifndef NRFCLAW_LORA_PROFILE_H
#define NRFCLAW_LORA_PROFILE_H
#include <stdbool.h>
#include <stdint.h>

#define NRFCLAW_LORA_DEFAULT_FREQUENCY_HZ 915000000UL
#define NRFCLAW_LORA_DEFAULT_POWER_DBM    14
#define NRFCLAW_LORA_DEFAULT_SF           7
#define NRFCLAW_LORA_DEFAULT_BW_KHZ       125
#define NRFCLAW_LORA_DEFAULT_CR           1
#define NRFCLAW_LORA_DEFAULT_SYNC_WORD    0x12U
#define NRFCLAW_LORA_DEFAULT_PREAMBLE     8U

typedef struct {
    uint32_t frequency_hz;    /* LLCC68 supported operating range: 150..960 MHz */
    int8_t power_dbm;         /* -9..+22 dBm */
    uint8_t sf;               /* LLCC68: SF depends on bandwidth */
    uint16_t bw_khz;          /* 125,250,500 */
    uint8_t cr;               /* 1..4 => 4/5..4/8 */
    uint8_t sync_word;        /* RadioLib-compatible LoRa sync word, default private 0x12 */
    uint16_t preamble_symbols;/* 1..65535, default 8 */
} nrfclaw_lora_profile_t;

void nrfclaw_lora_profile_default(nrfclaw_lora_profile_t *p);
bool nrfclaw_lora_profile_validate(nrfclaw_lora_profile_t const *p);
#endif
