# B8.1f — LLCC68 PHY Mapping

B8.1f maps B8.1e evidence into concrete LLCC68 PHY candidates where possible.

The central WH51 decision is intentionally mechanical and reviewable:

```text
external reference RX BW = 81.5 kHz
LLCC68 supported values around it:
    78.2 kHz  (0x1B)  narrower than reference
    93.8 kHz  (0x13)  first supported value >= reference
   117.3 kHz  (0x0B)  wider fallback
```

Primary mapping:

```text
WH51:
    bitrate      17.241 kbps
    deviation    30 kHz
    modulation   2-FSK
    shaping      none
    RX BW        93.8 kHz
    RX BW code   0x13
    sync         2D D4
    packet RX    fixed 14 bytes
```

The fixed 14-byte mode is an LLCC68 **receive implementation policy** based
on the exact payload size consumed by the decoder. It is not a claim that the
commercial transmitter itself uses fixed-length packet-engine mode.

For screening only, B8.1f calculates:

```text
occupied estimate = bitrate + 2 * deviation
```

For WH51 this is about 77.241 kHz. With 93.8 kHz RX BW, the simple total
margin is about 16.559 kHz, or about 8.2795 kHz per side. This is not a
hardware-validation result. 117.3 kHz is retained as the first wider fallback
if the 93.8 kHz synthetic/live receive gate is not robust enough.

Readiness:

- `LLCC68_EXECUTABLE_CANDIDATE` means all required LLCC68 PHY fields are
  mapped, but hardware has not yet validated this candidate.
- `SYNTHETIC_OTA_VALIDATED` remains reserved for an already passed synthetic
  LLCC68 OTA gate.
- No host mapping can set `LIVE_SENSOR_VALIDATED`.

Run:

```bash
python3 tools/map_llcc68_phy.py \
    --evidence build/b81e/rf_profile_evidence.json \
    --out-dir build/b81f \
    --show
```

Outputs:

- `llcc68_phy_mapping.json`
- `llcc68_phy_mapping.csv`
- `llcc68_gfsk_bandwidth_table.csv`
- `llcc68_phy_mapping.txt`
