# B8.1a — rtl_433 FSK Catalog Importer

This tool builds an **FSK-only metadata catalog** from a local
`rtl_433_ESP` checkout.

It intentionally imports only factual `r_device` metadata required for
nRFClaw compatibility analysis:

- device symbol and display name
- FSK modulation/line-code class
- nominal timing fields
- decoder/field symbol names
- source file and Git commit provenance

It does **not** copy decoder source code, decoder comments, payload examples,
or implementation logic into nRFClaw.

Accepted modulation classes are:

- `FSK_PULSE_PCM` → `NRZ`
- `FSK_PULSE_PWM` → `PWM`
- `FSK_PULSE_MANCHESTER_ZEROBIT` → `MANCHESTER_ZEROBIT`

OOK entries are excluded unconditionally.

## Usage

Clone or update `rtl_433_ESP` separately, then run:

```bash
python3 tools/import_rtl433_fsk_catalog.py \
    --source /path/to/rtl_433_ESP \
    --out-dir build/b81a
```

For the rtl_433_ESP snapshot that advertises 153 FSK registrations:

```bash
python3 tools/import_rtl433_fsk_catalog.py \
    --source /path/to/rtl_433_ESP \
    --out-dir build/b81a \
    --expect-count 153
```

Outputs:

- `build/b81a/rtl433_fsk_catalog.json`
- `build/b81a/rtl433_fsk_catalog.csv`
- `build/b81a/rtl433_fsk_catalog.txt`

The JSON is the input planned for B8.1b. B8.1a does not claim that an
imported FSK decoder is LLCC68-compatible.

## Self-test

```bash
python3 tools/import_rtl433_fsk_catalog.py --self-test
```
