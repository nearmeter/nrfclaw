#ifndef NRFCLAW_SHA256_H
#define NRFCLAW_SHA256_H

#include <stdint.h>
#include <stddef.h>

void nrfclaw_sha256(uint8_t const *data, size_t len, uint8_t out[32]);

#endif
