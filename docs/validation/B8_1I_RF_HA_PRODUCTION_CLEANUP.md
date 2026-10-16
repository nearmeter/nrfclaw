# B8.1i — RF/HA Production Cleanup

Scope: CLI + Home Assistant presentation only. No firmware/radio ABI change.

## Changes

1. `gfsk-capabilities` validates the capability schema for the active RF
   profile instead of comparing values with the old VEVOR synthetic vector.

2. Home Assistant presentation semantics:
   - `0x0920` battery voltage (mV)
   - `0x0921` soil moisture (%)
   - `0x0922` TX boost (diagnostic)
   - `0x0923` soil AD raw (diagnostic)
   - `0x0924` conductivity (µS/cm)

3. RF virtual nodes use the RF profile encoded in the current
   `0xF8ppssss` virtual-node ABI:
   - 1: VEVOR YT60234
   - 2: Ambient WH31E
   - 3: Ecowitt WH51
   - 4: Ecowitt WH52

The existing 4-bit profile-ID / low-16-bit sensor-ID virtual-node identity is
not redesigned in B8.1i.

B8.1i does not set `HARDWARE_VALIDATED`; real commercial-sensor validation
remains a separate gate.
