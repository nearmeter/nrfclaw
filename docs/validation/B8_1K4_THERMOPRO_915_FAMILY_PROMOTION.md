# B8.1k4 — ThermoPro 915 MHz Family Promotion

Profiles:
- TP211B: 33
- TP28B: 34
- TempSpike XR TP862B/TP863B: 35

Source decoders:
- `src/rtl_433/devices/thermopro_tp211b.c`
- `src/rtl_433/devices/thermopro_tp28b.c`
- `src/rtl_433/devices/thermopro_tp86xb.c`

## Runtime projection

TP211B:
- RF device ID
- Battery OK
- Temperature channel 0

TP28B:
- RF device ID
- Probe 1 temperature channel 0
- Probe 2 temperature channel 1

TempSpike XR:
- RF device ID
- Internal temperature channel 0
- Ambient temperature channel 1

Alarm thresholds/status flags and TempSpike docking/battery-bar diagnostics stay
documented in rtl_433 but are intentionally not projected in B8.1k4.

## Candidate LLCC68 PHY

- 915.000 MHz
- deviation 40 kHz
- RX bandwidth 117.3 kHz (`0x0B`)
- HARDWARE_VALIDATED remains clear

No CC1101 transmission gate is required.
