#ifndef NRFCLAW_AUTH_KEY_H
#define NRFCLAW_AUTH_KEY_H

/*
 * Stage-5 DEVELOPMENT KEY.
 *
 * Replace these 32 bytes before production.
 *
 * The Python CLI default key matches this value:
 *
 *   6e5246436c61772d5354414745352d4445562d4b45592d303030303030303031
 *
 * Under the Stage-5 threat model, physical access/button press is already
 * considered a security boundary failure. HMAC is used here specifically to
 * prevent accidental/unauthorized bytecode creation or modification.
 */
static const unsigned char NRFCLAW_AUTH_KEY[32] =
{
    0x6e,0x52,0x46,0x43,0x6c,0x61,0x77,0x2d,
    0x53,0x54,0x41,0x47,0x45,0x35,0x2d,0x44,
    0x45,0x56,0x2d,0x4b,0x45,0x59,0x2d,0x30,
    0x30,0x30,0x30,0x30,0x30,0x30,0x31,0x00
};

#endif
