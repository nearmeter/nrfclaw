# B8.1b2 — Band-Agnostic Protocol Compatibility

B8.1b2 corrects the previous assumption that the currently populated
900 MHz radio module should influence protocol compatibility.

Protocol compatibility and hardware RF range are separate concerns.

## Rules

Frequency is protocol/profile metadata only.

A protocol may therefore carry one or more known regional RF variants such
as:

- 433.92 MHz
- 868 MHz
- 915 MHz
- 916.45 MHz

None of those frequencies exclude the protocol from the nRFClaw catalog.

The hardware/module frequency gate is deferred to a later revision where the
populated NINASENSE radio module can be checked against the exact requested
profile frequency.

## Runtime policy

An external-RF bridge owns exactly one RF configuration at runtime:

```text
protocol/model
    ↓
selected RF variant
    ↓
exact center frequency
    ↓
LLCC68 remains on that frequency until reconfigured
```

There is no automatic frequency scan and no radio timesharing.

Several logical sensor models may still share one active radio configuration
when their PHY is identical, because multiple protocol/RFVM matchers can be
run over the same received frame without retuning the radio.

## Classes

B8.1b2 emits:

- `PACKET_NATIVE_CANDIDATE`
- `PACKET_NATIVE_REVIEW`
- `LINECODE_REQUIRED`
- `PULSE_TIMING_REQUIRED`
- `HOPPING_REQUIRED`

There is intentionally no `OUT_OF_BAND_CURRENT_MODULE` class.

## Run

```bash
python3 tools/classify_rtl433_fsk_llcc68.py \
    --catalog build/b81a/rtl433_fsk_catalog.json \
    --source /tmp/rtl_433_ESP \
    --out-dir build/b81b2
```

Inspect packet-native protocols:

```bash
python3 tools/classify_rtl433_fsk_llcc68.py \
    --catalog build/b81a/rtl433_fsk_catalog.json \
    --source /tmp/rtl_433_ESP \
    --out-dir build/b81b2 \
    --show-class PACKET_NATIVE_CANDIDATE
```

The generated JSON also records the design policy:

- frequency does not affect protocol compatibility;
- hardware frequency compatibility is deferred;
- one exact runtime center frequency;
- no automatic scan;
- no radio timesharing.
