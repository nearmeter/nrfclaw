#include "nrfclaw_lora_profile.h"

void nrfclaw_lora_profile_default(nrfclaw_lora_profile_t *p)
{
    if (!p) return;
    p->frequency_hz = NRFCLAW_LORA_DEFAULT_FREQUENCY_HZ;
    p->power_dbm = NRFCLAW_LORA_DEFAULT_POWER_DBM;
    p->sf = NRFCLAW_LORA_DEFAULT_SF;
    p->bw_khz = NRFCLAW_LORA_DEFAULT_BW_KHZ;
    p->cr = NRFCLAW_LORA_DEFAULT_CR;
    p->sync_word = NRFCLAW_LORA_DEFAULT_SYNC_WORD;
    p->preamble_symbols = NRFCLAW_LORA_DEFAULT_PREAMBLE;
}

bool nrfclaw_lora_profile_validate(nrfclaw_lora_profile_t const *p)
{
    if (!p) return false;
    if (p->frequency_hz < 150000000UL || p->frequency_hz > 960000000UL) return false;
    if (p->power_dbm < -9 || p->power_dbm > 22) return false;
    if (p->cr < 1U || p->cr > 4U) return false;
    if (p->preamble_symbols == 0U) return false;

    /* LLCC68 limits mirrored from RadioLib LLCC68::setSpreadingFactor(). */
    if (p->bw_khz == 125U) return p->sf >= 5U && p->sf <= 9U;
    if (p->bw_khz == 250U) return p->sf >= 5U && p->sf <= 10U;
    if (p->bw_khz == 500U) return p->sf >= 5U && p->sf <= 11U;
    return false;
}
