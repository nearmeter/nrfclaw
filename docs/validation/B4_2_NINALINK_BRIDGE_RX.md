# B4.2 — NinaLink Bridge RX

## Goal

Move NinaLink validation from the host into the NINASENSE bridge firmware.

```text
LLCC68 continuous RX
        ↓
64-byte RF stream queue
        ↓
B3.1 nrfclaw_ninalink_decode()
        ↓
CRC/header/version/length validation
        ↓
B3.2 CAP_REPORT/CAP_EVENT semantic parser
        ↓
validated bridge queue
        ↓
chunked NDP/NUS transport
        ↓
CLI display
```

Only frames accepted by the on-device bridge validator enter the bridge queue.

## RF size

B4.2 raises the internal continuous LoRa stream packet storage from 48 to
64 bytes, matching `NRFCLAW_NINALINK_MAX_FRAME_SIZE`.

This does not enlarge the legacy one-notification NUS diagnostic response.
The bridge exports a validated RF frame in 8-byte chunks, keeping each NDP
response within the validated 20-byte ATT payload.

## Validation policy

Every RF packet first passes B3.1:

- magic
- version
- flags
- encoded length
- maximum 64-byte frame
- CRC16/CCITT-FALSE

`CAP_REPORT` and `CAP_EVENT` additionally pass the B3.2 typed-value parser.
Malformed value-entry layouts are rejected on-device.

Other core-valid message types are preserved. This follows the NinaLink v1
rule that unknown transport-level message types are not rejected solely because
the bridge does not yet interpret them.

## Bridge queue

The validated queue depth is four complete NinaLink frames. Counters expose:

- received RF packets
- valid NinaLink frames
- invalid NinaLink frames
- validated-queue drops
- underlying continuous-RX queue drops
- last bridge error

## NDP lab control

B4.2 adds physical-NUS-only opcode:

```text
0x5A NINALINK_BRIDGE
```

Actions:

```text
00 status
01 start
02 stop
03 take next validated frame in 8-byte chunks
```

Status is deliberately compact enough for one 20-byte NUS notification.

## CLI

Start the mains-powered bridge:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-start
```

The bridge keeps listening after that NUS connection closes.

Inspect status:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-status
```

Drain/display validated frames:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
    ninalink-bridge-rx --timeout 45
```

Stop:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-stop
```

## B4.2 gate

PASS requires:

1. B3.1 host codec tests PASS.
2. B3.2 semantic tests PASS.
3. ARM build and `git diff --check` PASS.
4. Existing HA compatibility remains PASS.
5. Bridge start succeeds and survives NUS disconnect.
6. 28-byte B4.1 CAP_REPORTs are accepted on-device.
7. At least three consecutive reports are delivered by the validated queue.
8. Sequence increments.
9. Battery and temperature decode correctly at the host.
10. Bridge status reports `valid >= 3`, `invalid = 0`, `dropped = 0`.
11. A deliberately non-NinaLink LoRa packet increments `invalid` and is not
    delivered through `ninalink-bridge-rx`.
12. The bridge path is capable of storing/exporting a full 64-byte validated
    NinaLink frame without the old 48-byte RF queue limitation.

ACK, retransmission, downlink and Home Assistant child-device creation remain
outside B4.2.


## Maximum 64-byte frame gate

The sender command:

```bash
ninalink-tx-max
```

transmits exactly one valid 64-byte `CAP_REPORT`:

```text
13-byte header
49-byte payload
 2-byte CRC
--------------
64 bytes
```

The payload contains six U32 semantic entries. The B4.2 bridge must therefore
pass both B3.1 core validation and B3.2 value parsing before the frame is added
to the validated queue.
