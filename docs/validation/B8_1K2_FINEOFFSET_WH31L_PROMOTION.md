# B8.1k2 — Fine Offset / Ambient Weather WH31L (WH57)

Profile ID: **14**
Frequency: **915.000 MHz**

## Source-derived decoder contract

From `src/rtl_433/devices/fineoffset_wh31l.c` in the local rtl_433_ESP checkout:

- FSK PCM
- 56 us bit time (~17.857 kbps)
- 40 alternating preamble bits
- sync `2D D4`
- fixed 9-byte payload
- type `0x57`
- CRC-8 polynomial `0x31`, init `0x00`, zero remainder over bytes 0..7
- SUM-8 of bytes 0..7 equals byte 8
- 20-bit device ID
- battery level encoded as 0 / 0.5 / 1.0
- lightning distance 1..40 km; 63 means invalid/no strike
- 8-bit lightning strike count

The source examples used by the host gate are:

```text
57 81 05 C8 05 81 01 DF 0B
57 01 05 C8 05 BF 00 DD C6
```

## nRFClaw candidate PHY

The rtl_433 decoder does not specify frequency deviation or receiver filter
bandwidth. B8.1k2 therefore uses the already-established nRFClaw 56-us Fine
Offset candidate tuple:

- deviation: 40 kHz
- LLCC68 RX bandwidth: 117.3 kHz (`0x0B`)

This is a runtime candidate, not a live-sensor validation.

## NinaLink / HA capabilities

- `0x0900` RF device ID
- `0x0002` battery percentage
- `0x0936` lightning count
- `0x0937` lightning distance (km)

Distance value 63 is explicitly invalid in rtl_433. The RFVM therefore omits
`0x0937` for such a frame instead of publishing a fabricated 63 km reading.

`HARDWARE_VALIDATED` remains clear until a commercial WH31L/WH57 is tested.
