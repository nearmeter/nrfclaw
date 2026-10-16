# B8.1k7d2 — NetAtmo NINASENSE-to-NINASENSE PHY Isolation

Purpose: remove the CC1101 fixture from the intermittent NetAtmo PHY failure.

Selector 26 gains the canonical 52-byte WIND payload for profile 27.

TX NINASENSE uses the production profile-27 registry tuple:
- 868.950 MHz
- 117647 bps
- +/-50 kHz
- sync AA E7 12
- fixed 52-byte payload
- radio CRC off

This is a diagnostic isolation gate only. RX/runtime/HA are unchanged and
HARDWARE_VALIDATED remains false.
