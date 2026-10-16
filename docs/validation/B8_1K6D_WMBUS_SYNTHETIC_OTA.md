# B8.1k6d — Wireless M-Bus Format-B Synthetic OTA Gate

The existing `rf-profile tx-test` diagnostic gains profile 37.

Canonical on-air payload after sync `54 3D 54 3D`:

    0C 44 2C 2D 78 56 34 12 01 02 78 8C A2

Properties:
- L = 0x0C
- physical payload = 13 bytes = L+1
- meter ID = 12345678
- EN13757 CRC = 0x8CA2

TX deliberately uses LLCC68 FIXED packet mode so L remains the first on-air
payload byte. RX remains B8.1k6c VARIABLE packet mode.

This synthetic two-NINASENSE gate does not set HARDWARE_VALIDATED. A real
Wireless M-Bus meter is still required for that status.
