# B4.1 — NinaLink Lab Uplink

Prerequisite: B3.2 commit `5ad821d`.

## Goal

Prove the first real on-air semantic path:

```text
B2 cached capabilities
        ↓
CAP_REPORT
        ↓
B3.2 semantic codec
        ↓
B3.1 NinaLink frame
        ↓
LLCC68 TX
        ↓
LLCC68 continuous RX
        ↓
existing physical-NUS diagnostic queue
        ↓
CLI NinaLink decoder
```

B4.1 intentionally does not implement ACK/downlink, production provisioning,
Home Assistant forwarding, or a persistent bridge role.

## Why the sender is armed autonomously

During B0, two simultaneous `nrfclaw_cli.py` processes triggered a BlueZ/Bleak
concurrent discovery/connect conflict. B4.1 therefore uses a RAM-only periodic
sender.

The CLI primes the battery/DS18B20 caches, arms a repeated firmware timer and
disconnects. The node then transmits without BLE, allowing a second NINASENSE to
be connected as the receiver. The first TX occurs one full period after arming.

## Lab report

The sender asks the B2 capability runtime for cached:

```text
battery_voltage
temperature
```

Only available values are included. A typical two-entry report is 28 RF bytes:
13-byte NinaLink header + 13-byte payload + 2-byte CRC.

This is below the existing diagnostic receive queue's 48-byte payload limit.
That 48-byte diagnostic limit is not the NinaLink protocol limit; B3 remains
64 bytes.

## NDP lab opcode

B4.1 adds physical-NUS-only opcode:

```text
0x59 NINALINK_LAB
```

Payload:

```text
00                    status
01 period_s:u16 LE    start/arm
02                    stop
```

## CLI

Sender:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
    ninalink-tx-lab --every 15
```

Receiver:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
    ninalink-rx --timeout 45
```

Stop:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 ninalink-tx-stop
```

Expected:

```text
NinaLink v1 CAP_REPORT net=0x0000 node=0xAD64D423 seq=1
  battery_voltage[0] = 3.300 V
  temperature[0] = 19.88 C
```

## Gate

B4.1 PASS requires:

1. B3.1/B3.2 host tests remain green.
2. ARM build passes.
3. HA compatibility remains PASS.
4. Sender continues after NUS disconnect.
5. Receiver gets at least two consecutive NinaLink reports.
6. Sequence increments.
7. CRC validates.
8. Battery/temperature decode with B1.1 semantics.
9. RSSI/SNR are reported.
10. continuous RX rearms.

## Deliberate limits

- network_id = 0x0000;
- sequence is RAM-only;
- no ACK/retry;
- cached values are primed before arming;
- current NUS diagnostic bridge is limited to 48 RF bytes.

B4.2 will introduce proper bridge handling for complete 64-byte NinaLink frames
and on-device NinaLink validation.


## B4.1 NUS transport correction

The B0 direct `lora-rx` diagnostic queue can store up to 48 RF bytes, but the
validated physical NUS transport uses ATT_MTU=23 and therefore one notification
carries at most 20 bytes.

The legacy NDP `LORA_DIAG_RX` response uses 11 bytes of total overhead:

```text
5 bytes NDP response overhead
6 bytes LoRa RX metadata
```

So only 9 RF bytes fit in one legacy notification.

B4.1 adds `LORA_DIAG_RX action=3`, fragmenting one queued RF packet into
8-byte chunks while preserving RSSI/SNR. `ninalink-rx` reassembles the packet
before NinaLink CRC validation and semantic decoding. Legacy action=1 remains
unchanged.
