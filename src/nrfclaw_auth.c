#include "nrfclaw_auth.h"
#include "nrfclaw_auth_key.h"

#include <string.h>

typedef struct
{
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} sha256_ctx_t;

static const uint32_t k[64] =
{
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

#define ROTR(x,n) (((x) >> (n)) | ((x) << (32U-(n))))
#define CH(x,y,z) (((x)&(y)) ^ (~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y)) ^ ((x)&(z)) ^ ((y)&(z)))
#define EP0(x) (ROTR((x),2) ^ ROTR((x),13) ^ ROTR((x),22))
#define EP1(x) (ROTR((x),6) ^ ROTR((x),11) ^ ROTR((x),25))
#define SIG0(x) (ROTR((x),7) ^ ROTR((x),18) ^ ((x)>>3))
#define SIG1(x) (ROTR((x),17) ^ ROTR((x),19) ^ ((x)>>10))

static void sha256_transform(sha256_ctx_t *ctx, uint8_t const data[64])
{
    uint32_t m[64];
    uint32_t a,b,c,d,e,f,g,h,t1,t2;

    for (uint32_t i=0,j=0; i<16; ++i,j+=4)
    {
        m[i] = ((uint32_t)data[j] << 24) |
               ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) |
               ((uint32_t)data[j+3]);
    }

    for (uint32_t i=16; i<64; ++i)
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];

    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];

    for (uint32_t i=0; i<64; ++i)
    {
        t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }

    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->datalen=0;
    ctx->bitlen=0;
    ctx->state[0]=0x6a09e667UL; ctx->state[1]=0xbb67ae85UL;
    ctx->state[2]=0x3c6ef372UL; ctx->state[3]=0xa54ff53aUL;
    ctx->state[4]=0x510e527fUL; ctx->state[5]=0x9b05688cUL;
    ctx->state[6]=0x1f83d9abUL; ctx->state[7]=0x5be0cd19UL;
}

static void sha256_update(sha256_ctx_t *ctx, uint8_t const *data, uint32_t len)
{
    for (uint32_t i=0; i<len; ++i)
    {
        ctx->data[ctx->datalen++] = data[i];

        if (ctx->datalen == 64U)
        {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512U;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;

    if (ctx->datalen < 56U)
    {
        ctx->data[i++] = 0x80U;
        while (i < 56U) ctx->data[i++] = 0x00U;
    }
    else
    {
        ctx->data[i++] = 0x80U;
        while (i < 64U) ctx->data[i++] = 0x00U;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56U);
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8ULL;

    ctx->data[63]=(uint8_t)(ctx->bitlen);
    ctx->data[62]=(uint8_t)(ctx->bitlen>>8);
    ctx->data[61]=(uint8_t)(ctx->bitlen>>16);
    ctx->data[60]=(uint8_t)(ctx->bitlen>>24);
    ctx->data[59]=(uint8_t)(ctx->bitlen>>32);
    ctx->data[58]=(uint8_t)(ctx->bitlen>>40);
    ctx->data[57]=(uint8_t)(ctx->bitlen>>48);
    ctx->data[56]=(uint8_t)(ctx->bitlen>>56);

    sha256_transform(ctx, ctx->data);

    for (i=0; i<4; ++i)
    {
        hash[i]      =(uint8_t)((ctx->state[0]>>(24U-i*8U))&0xffU);
        hash[i+4]    =(uint8_t)((ctx->state[1]>>(24U-i*8U))&0xffU);
        hash[i+8]    =(uint8_t)((ctx->state[2]>>(24U-i*8U))&0xffU);
        hash[i+12]   =(uint8_t)((ctx->state[3]>>(24U-i*8U))&0xffU);
        hash[i+16]   =(uint8_t)((ctx->state[4]>>(24U-i*8U))&0xffU);
        hash[i+20]   =(uint8_t)((ctx->state[5]>>(24U-i*8U))&0xffU);
        hash[i+24]   =(uint8_t)((ctx->state[6]>>(24U-i*8U))&0xffU);
        hash[i+28]   =(uint8_t)((ctx->state[7]>>(24U-i*8U))&0xffU);
    }
}

static void hmac_sha256(uint8_t const *key, uint32_t key_len,
                        uint8_t const *part1, uint32_t part1_len,
                        uint8_t const *part2, uint32_t part2_len,
                        uint8_t const *part3, uint32_t part3_len,
                        uint8_t out[32])
{
    uint8_t key_block[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];

    memset(key_block,0,sizeof(key_block));

    if (key_len > 64U)
    {
        sha256_ctx_t tmp;
        sha256_init(&tmp);
        sha256_update(&tmp,key,key_len);
        sha256_final(&tmp,key_block);
    }
    else
    {
        memcpy(key_block,key,key_len);
    }

    for (uint32_t i=0;i<64U;++i)
    {
        ipad[i]=(uint8_t)(key_block[i]^0x36U);
        opad[i]=(uint8_t)(key_block[i]^0x5cU);
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx,ipad,64U);
    if (part1 && part1_len) sha256_update(&ctx,part1,part1_len);
    if (part2 && part2_len) sha256_update(&ctx,part2,part2_len);
    if (part3 && part3_len) sha256_update(&ctx,part3,part3_len);
    sha256_final(&ctx,inner);

    sha256_init(&ctx);
    sha256_update(&ctx,opad,64U);
    sha256_update(&ctx,inner,32U);
    sha256_final(&ctx,out);

    memset(key_block,0,sizeof(key_block));
    memset(ipad,0,sizeof(ipad));
    memset(opad,0,sizeof(opad));
    memset(inner,0,sizeof(inner));
}

void nrfclaw_auth_compute(uint8_t const *program,
                          uint16_t len,
                          nrfclaw_schedule_t const *schedule,
                          uint8_t tag[NRFCLAW_AUTH_TAG_SIZE])
{
    static const uint8_t domain[] = "nRFClaw-bytecode-v6";

    uint8_t metadata[2U + NRFCLAW_SCHEDULE_WIRE_SIZE];
    uint8_t schedule_wire[NRFCLAW_SCHEDULE_WIRE_SIZE];

    metadata[0] = (uint8_t)(len & 0xffU);
    metadata[1] = (uint8_t)(len >> 8);

    nrfclaw_schedule_encode(schedule, schedule_wire);
    memcpy(&metadata[2], schedule_wire, NRFCLAW_SCHEDULE_WIRE_SIZE);

    hmac_sha256(
        (uint8_t const *)NRFCLAW_AUTH_KEY,
        NRFCLAW_AUTH_KEY_SIZE,
        domain,
        sizeof(domain),
        metadata,
        sizeof(metadata),
        program,
        len,
        tag
    );

    memset(metadata, 0, sizeof(metadata));
    memset(schedule_wire, 0, sizeof(schedule_wire));
}

bool nrfclaw_auth_verify(uint8_t const *program,
                         uint16_t len,
                         nrfclaw_schedule_t const *schedule,
                         uint8_t const tag[NRFCLAW_AUTH_TAG_SIZE])
{
    uint8_t expected[NRFCLAW_AUTH_TAG_SIZE];
    uint8_t diff = 0U;

    if (!program || !schedule || !tag || len == 0U)
        return false;

    nrfclaw_auth_compute(program, len, schedule, expected);

    for (uint32_t i = 0; i < NRFCLAW_AUTH_TAG_SIZE; ++i)
        diff |= (uint8_t)(expected[i] ^ tag[i]);

    memset(expected, 0, sizeof(expected));
    return diff == 0U;
}
