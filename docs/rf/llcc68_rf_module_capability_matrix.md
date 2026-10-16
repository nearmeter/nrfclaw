# B8.1d — RF Module Capability Matrix + Exact-Frequency Runtime Gate

B8.1d separates **protocol support** from **the frequency range of the
populated NINASENSE radio module**.

Built-in NINASENSE module variants:

- `E220-400MM22S`: 410–493 MHz
- `E220-900MM22S`: 850–930 MHz

A protocol is never removed from the catalog because of module range.

Instead, every exact RF variant emitted by B8.1c1 is evaluated against the
selected hardware module:

```text
logical protocol
  ↓
exact RF variant
  ↓
module capability gate
  ↓
configure LLCC68 to that one frequency
```

Examples:

```text
WH52 @ 433.920 MHz + E220-400MM22S -> SUPPORTED
WH52 @ 433.920 MHz + E220-900MM22S -> OUTSIDE_MODULE_RANGE

WH52 @ 915.000 MHz + E220-900MM22S -> SUPPORTED
WH52 @ 915.000 MHz + E220-400MM22S -> OUTSIDE_MODULE_RANGE
```

`OUTSIDE_MODULE_RANGE` applies only to that RF variant on that hardware. It
does not mean the protocol is unsupported by nRFClaw.

Runtime policy remains:

- exactly one selected center frequency;
- no automatic frequency scan;
- no radio timesharing;
- stay on the selected frequency until explicitly reconfigured.

## Run

900 MHz module:

```bash
python3 tools/check_llcc68_rf_module_compat.py \
    --seeds build/b81c1/rtl433_llcc68_packet_seeds.json \
    --module E220-900MM22S \
    --out-dir build/b81d \
    --show
```

400 MHz module:

```bash
python3 tools/check_llcc68_rf_module_compat.py \
    --seeds build/b81c1/rtl433_llcc68_packet_seeds.json \
    --module E220-400MM22S \
    --out-dir build/b81d \
    --show
```

Inspect one protocol:

```bash
python3 tools/check_llcc68_rf_module_compat.py \
    --seeds build/b81c1/rtl433_llcc68_packet_seeds.json \
    --module E220-900MM22S \
    --out-dir build/b81d \
    --protocol WH52
```
