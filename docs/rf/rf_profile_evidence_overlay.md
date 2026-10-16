# B8.1e — Evidence-Based RF Profile Overlay

B8.1e enriches the B8.1c1 packet seeds with radio parameters only when there
is concrete evidence.

It deliberately separates four readiness concepts:

- `PARTIAL`
  - some framing/protocol facts are known, but radio parameters are incomplete
- `REFERENCE_PHY`
  - an external/reference radio configuration exists
  - this is **not** an LLCC68 validation result
- `LLCC68_EXECUTABLE_CANDIDATE`
  - a complete LLCC68 configuration exists but has not yet passed hardware
- `SYNTHETIC_OTA_VALIDATED`
  - the nRFClaw LLCC68 synthetic OTA gate passed
  - this is **not** the same as receiving the real commercial sensor

The host tool never sets `live_sensor_validated=true`.

Initial evidence set:

- VEVOR 7-in-1
  - measured 915 MHz PHY from rtl_433_ESP
  - existing nRFClaw LLCC68 synthetic OTA profile
- Fine Offset / Ecowitt WH51
  - rtl_433 decoder framing/timing
  - rtl_433_ESP CC1101 reference-transmitter PHY
- Fine Offset / Ecowitt WH52
  - regional frequencies, 58 us NRZ, WH51-family framing, 24-byte payload
  - deviation/RX BW intentionally unresolved
- Ambient Weather WH31E
  - 915 MHz, 56 us NRZ, 48-bit alternating preamble, 2D D4 sync, 11-byte payload
  - deviation/RX BW intentionally unresolved

Run:

```bash
python3 tools/build_rf_profile_evidence.py \
    --seeds build/b81c1/rtl433_llcc68_packet_seeds.json \
    --out-dir build/b81e \
    --show
```

The next revision can use this overlay to decide which profiles are safe to
materialize into the firmware registry and which still need an LLCC68-specific
radio-parameter mapping or a live-sensor gate.
