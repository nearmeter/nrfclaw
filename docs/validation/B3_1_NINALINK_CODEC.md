# B3.1 — NinaLink Core Codec Gate

Status: candidate implementation.

## Scope

B3.1 implements only the generic NinaLink v1 frame codec frozen by B1.2:

- fixed 13-byte header;
- little-endian integer fields;
- payload length 0..49;
- total frame length 15..64;
- CRC-16/CCITT-FALSE;
- validation of magic, version, implemented flags, physical length and CRC;
- preservation of unknown message types.

It does **not** implement:

- HELLO payload construction;
- capability descriptors;
- CAP_REPORT/CAP_EVENT entries;
- ACK/NACK semantics beyond their message-type constants;
- sequence allocation/retry policy;
- LLCC68 TX/RX;
- bridge mode;
- BLE/NDP;
- Home Assistant.

Those are B3.2/B4 work.

## Files

New:

```text
include/nrfclaw_ninalink.h
src/nrfclaw_ninalink.c
tests/host/test_ninalink_codec.c
tests/host/run_ninalink_codec.sh
docs/validation/B3_1_NINALINK_CODEC.md
```

Modified:

```text
Makefile
```

The Makefile change only adds `src/nrfclaw_ninalink.c` to `CORE_SRC`.
Nothing calls it from firmware runtime yet, so `--gc-sections` should remove it
from the final image.

## Frozen golden vector

Logical frame:

```text
flags        = ACK_REQ
message_type = CAP_REPORT (0x10)
network_id   = 0x1234
node_id      = 0x12345678
sequence     = 0xABCD
payload      = AA 55 01
```

Expected on-wire bytes:

```text
4E 01 01 10 34 12 03 78 56 34 12 CD AB AA 55 01 C9 59
```

The last two bytes are the little-endian CRC value `0x59C9`.

This is a fixed vector, not a self-generated expected value.

## CRC reference vector

For ASCII:

```text
123456789
```

CRC-16/CCITT-FALSE must equal:

```text
0x29B1
```

## Required host tests

Run:

```bash
./tests/host/run_ninalink_codec.sh
```

The suite checks:

- standard CRC vector;
- exact golden encoded bytes;
- golden decode;
- maximum 64-byte frame;
- unknown message-type preservation;
- bad CRC;
- truncation;
- declared-length mismatch;
- bad magic;
- unsupported protocol version;
- reserved AUTH flag;
- reserved ENCRYPTED flag;
- payload >49;
- insufficient output buffer;
- physical frame >64;
- physical frame <15.

Expected final line:

```text
B3.1 NinaLink codec: PASS
```

## Firmware build gate

After host tests:

```bash
make clean
make -j$(nproc)
make size
git diff --check
```

Because no runtime code references the codec yet, expected final image size is
still the B0/B2 reference:

```text
text 170672
data    604
bss    8184
```

A size increase is not automatically a failure, but must be explained before
B3.1 is accepted.

## Runtime regression gate

B3.1 must not modify:

```text
src/main.c
src/nrfclaw_lora.c
src/nrfclaw_ndp.c
src/nrfclaw_vm.c
```

Optional but recommended after flashing:

```bash
cd NRFCLAW_CLI
python3 nrfclaw_cli.py --device 1BF1D0 ha test
```

Expected:

```text
HA compatibility: PASS
```

## Acceptance

B3.1 is PASS when:

1. host tests pass;
2. ARM firmware builds cleanly;
3. `git diff --check` is clean;
4. legacy runtime files are untouched;
5. B0/HA behavior remains unchanged;
6. the golden vector remains frozen.

Suggested commit:

```text
B3.1: add NinaLink v1 core codec
```
