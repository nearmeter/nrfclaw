#include "nrfclaw_sha256.h"

#include <string.h>

typedef struct
{
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t used;
} sha256_ctx_t;

static uint32_t rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static uint32_t be32(uint8_t const *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void block(sha256_ctx_t *c, uint8_t const b[64])
{
    static const uint32_t k[64] = {
        0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,
        0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
        0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,
        0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
        0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,
        0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
        0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,
        0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
        0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,
        0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
        0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,
        0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
        0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,
        0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
        0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
        0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
    };

    uint32_t w[64];

    for (uint32_t i = 0; i < 16U; i++)
        w[i] = be32(&b[i * 4U]);

    for (uint32_t i = 16U; i < 64U; i++)
    {
        uint32_t s0 = rotr(w[i-15U],7U) ^ rotr(w[i-15U],18U) ^ (w[i-15U] >> 3U);
        uint32_t s1 = rotr(w[i-2U],17U) ^ rotr(w[i-2U],19U) ^ (w[i-2U] >> 10U);
        w[i] = w[i-16U] + s0 + w[i-7U] + s1;
    }

    uint32_t a=c->h[0], b0=c->h[1], d=c->h[3], e=c->h[4];
    uint32_t f=c->h[5], g=c->h[6], h=c->h[7], cc=c->h[2];

    for (uint32_t i = 0; i < 64U; i++)
    {
        uint32_t S1 = rotr(e,6U) ^ rotr(e,11U) ^ rotr(e,25U);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = rotr(a,2U) ^ rotr(a,13U) ^ rotr(a,22U);
        uint32_t maj = (a & b0) ^ (a & cc) ^ (b0 & cc);
        uint32_t t2 = S0 + maj;

        h=g; g=f; f=e; e=d+t1;
        d=cc; cc=b0; b0=a; a=t1+t2;
    }

    c->h[0]+=a; c->h[1]+=b0; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void init(sha256_ctx_t *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,
        0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL
    };
    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0U;
    c->used = 0U;
}

static void update(sha256_ctx_t *c, uint8_t const *data, size_t len)
{
    while (len)
    {
        uint32_t room = 64U - c->used;
        uint32_t n = (len < room) ? (uint32_t)len : room;
        memcpy(&c->buf[c->used], data, n);
        c->used += n;
        c->bits += (uint64_t)n * 8ULL;
        data += n;
        len -= n;

        if (c->used == 64U)
        {
            block(c, c->buf);
            c->used = 0U;
        }
    }
}

static void final(sha256_ctx_t *c, uint8_t out[32])
{
    c->buf[c->used++] = 0x80U;

    if (c->used > 56U)
    {
        while (c->used < 64U) c->buf[c->used++] = 0U;
        block(c, c->buf);
        c->used = 0U;
    }

    while (c->used < 56U) c->buf[c->used++] = 0U;

    for (uint32_t i = 0; i < 8U; i++)
        c->buf[63U - i] = (uint8_t)(c->bits >> (i * 8U));

    block(c, c->buf);

    for (uint32_t i = 0; i < 8U; i++)
        put_be32(&out[i * 4U], c->h[i]);
}

void nrfclaw_sha256(uint8_t const *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t c;
    init(&c);
    update(&c, data, len);
    final(&c, out);
}
