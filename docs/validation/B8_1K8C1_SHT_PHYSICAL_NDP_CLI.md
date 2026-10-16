# B8.1k8c1 — SHT Physical NDP/CLI Diagnostic

Purpose: validate the B8.1k8c SHT family driver on real NINASENSE hardware
before exposing SHT measurements through the logical temperature provider,
NinaLink, or Home Assistant.

## Scope

Physical-NUS-only NDP opcode 100 / 0x64.

Selectors:

- `0`: info
- `1`: configure + probe; payload may be only selector for default D11/D14,
  or selector + SCL + SDA for an explicit alternate pair
- `2`: read temperature and humidity
- `3`: release software-I2C GPIO ownership

The diagnostic leaves a successfully configured SHT bus active after
`sht-detect`; use `sht-release` when finished.

## CLI

Default connector pins:

    python3 NRFCLAW_CLI/nrfclaw_cli.py --device <ID> sht-detect
    python3 NRFCLAW_CLI/nrfclaw_cli.py --device <ID> sht-info
    python3 NRFCLAW_CLI/nrfclaw_cli.py --device <ID> sht-read
    python3 NRFCLAW_CLI/nrfclaw_cli.py --device <ID> sht-release

Alternate GPIO pair:

    python3 NRFCLAW_CLI/nrfclaw_cli.py --device <ID> \
        sht-detect --scl <GPIO> --sda <GPIO>

## Expected families

- SHT2x
- SHT3x
- SHT4x
- SHTC3

The driver reports family, I2C address, active SCL/SDA pins, serial when the
family probe supplies one, temperature in degC, and relative humidity in %RH.

No logical temperature-provider, NinaLink, Home Assistant, RF, or LIS2DH12
behavior is changed by B8.1k8c1.
