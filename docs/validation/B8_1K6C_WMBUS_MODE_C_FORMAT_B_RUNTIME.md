# B8.1k6c — Wireless M-Bus Mode C / Format B Runtime

Profile **37** is a derived exact-framing runtime profile.

The original rtl_433 profile 24 remains catalog-only because it combines:
Mode C Format A, Mode C Format B, and Mode T.

## PHY

- frequency: 868950000 Hz
- bitrate/chip rate: 100000
- deviation: +/-45000 Hz typical
- LLCC68 RX bandwidth: 467000 Hz (`0x09`)
- preamble: 32 bits
- sync: `54 3D 54 3D`
- packet mode: variable
- hardware validation: false

## Variable-length contract

Wireless M-Bus Format B starts with `L` and the physical telegram contains
`L+1` bytes.

In LLCC68 variable packet mode, the one-byte packet length field is consumed
by the packet engine. The nRFClaw radio backend therefore reconstructs it:

    RF FIFO payload length N
          -> prepend N as byte 0
          -> RFVM sees [L | telegram bytes]

The automatic ingest buffer is extended from 64 to 256 reconstructed bytes.
Existing B8.0m3a exact-frame diagnostics intentionally retain their 64-byte
trace storage; a longer newest frame invalidates that short-frame trace
instead of exposing stale data.

## RFVM acceptance

The Format-B validator mirrors rtl_433:

- `L >= 12`
- reconstructed length is exactly `L+1`
- CRC-16/EN-13757 polynomial `0x3D65`, init `0`, final complement
- first-block CRC
- second-block CRC when `L > 129`
- meter ID is the little-endian four-byte BCD field at offsets 4..7

Current capability projection:

- `0x0900` RF device ID

No application payload interpretation or decryption is invented in this gate.

## Hardware status

`HARDWARE_VALIDATED` remains clear.

After host/build PASS, configure a NINASENSE bridge with profile 37 and test
against a real C1/Format-B meter, or use a controlled/shielded bench RF source.
