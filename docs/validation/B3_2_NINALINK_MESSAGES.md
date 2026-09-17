# B3.2 — NinaLink Semantic Messages Gate

Status: candidate implementation.

Prerequisite:

```text
B3.1 commit: 14dc5d4
```

## Scope

B3.2 maps the semantic types frozen in B1.1/B2 onto the NinaLink frame codec
implemented by B3.1.

It implements payload builders/parsers for:

```text
HELLO
CAPS_REQUEST
CAPS_RESPONSE
CAP_REPORT
CAP_EVENT
ACK
NACK
```

It deliberately does not touch the LLCC68 or sensor acquisition state machines.

## B2 integration boundary

B3.2 includes:

```c
#include "nrfclaw_capability.h"
```

and directly transports:

```text
nrfclaw_capability_desc_t
nrfclaw_capability_value_t
```

This is the first NinaLink layer that consumes the B2 semantic ABI.

However, B3.2 does not call:

```text
nrfclaw_capability_state()
nrfclaw_capability_read_current()
```

Those calls belong to B4, where a node runtime decides what to advertise/report.

This separation keeps B3.2 deterministic and host-testable.

## Capability discovery

A `CAPS_RESPONSE` page contains:

```text
4-byte page header
5 x 9-byte descriptors
```

Maximum payload:

```text
4 + 45 = 49 bytes
```

Maximum complete frame:

```text
13 header + 49 payload + 2 CRC = 64 bytes
```

The host tests explicitly exercise this exact boundary.

## Reports and events

`CAP_REPORT` and `CAP_EVENT` use the same entry encoding:

```text
entry_count:u8

repeated:
  capability_id:u16
  channel:u8
  value_type:u8
  value:1/2/4 bytes
```

The capability ID may be unknown to the receiver. It is preserved verbatim.

The `value_type` must be understood in order to find the next entry boundary.
Therefore an unknown value type is rejected with:

```text
UNSUPPORTED_VALUE_TYPE
```

This does not violate the B1.1 unknown-capability rule: unknown capability IDs
remain forwardable as long as the typed value itself can be parsed.

## Value capacity

Six U32/S32 entries exactly fill a 49-byte payload:

```text
1 + 6 * (4 + 4) = 49
```

A seventh U32 entry is rejected before any frame is emitted.

## BOOL

Canonical BOOL wire values are:

```text
0 = false
1 = true
```

Any other received BOOL value is rejected as `BAD_VALUE`.

## Host gate

Run:

```bash
./tests/host/run_ninalink_messages.sh
```

Required final result:

```text
B3.2 NinaLink semantic messages: PASS
```

The suite covers:

- HELLO build/parse through B3.1 codec;
- five-descriptor CAPS page -> exact 64-byte NinaLink frame;
- CAPS pagination/MORE;
- custom/unknown capability descriptor preservation;
- CAPS_REQUEST;
- mixed CAP_REPORT fixed-point/integer/bool values;
- unknown capability ID preservation;
- six U32 entries at the exact payload limit;
- overflow rejection;
- CAP_EVENT;
- invalid BOOL;
- unknown value type;
- ACK/NACK;
- malformed/truncated semantic payloads;
- reserved descriptor byte validation.

## Firmware gate

```bash
make clean
make -j$(nproc)
make size
git diff --check
```

No runtime consumer exists yet, so the expected image remains:

```text
text 170672
data    604
bss    8184
```

because B3.1/B3.2 should still be removed by `--gc-sections`.

## Regression boundary

B3.2 must not modify:

```text
src/main.c
src/nrfclaw_lora.c
src/nrfclaw_ndp.c
src/nrfclaw_vm.c
src/nrfclaw_capability.c
```

## Acceptance

B3.2 is PASS when:

1. host tests pass;
2. ARM build passes;
3. `git diff --check` is clean;
4. legacy runtime remains untouched;
5. image size remains unchanged or any difference is explained;
6. B3.1 golden core vector remains passing.

Suggested commit:

```text
B3.2: add NinaLink semantic message codec
```

After B3.2, B3 is complete. B4 may then bind the semantic layer to actual node
and bridge runtime behavior.
