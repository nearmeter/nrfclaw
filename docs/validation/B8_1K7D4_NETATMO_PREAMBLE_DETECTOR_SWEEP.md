# B8.1k7d4a — NetAtmo Preamble-Detector Sweep regex corrective

Diagnostic-only follow-up to B8.1k7d3.

Primary hardware gate:
- 868.950 MHz
- 117647 bps
- +/-50 kHz
- RXBW 373600 Hz
- sync AA E7 12
- 52-byte fixed window
- LLCC68 CRC off
- TX fixture 1BF1D0 at -9 dBm

Detector sweep:
- 24 bits / 0x06 — baseline
- 16 bits / 0x05 — primary candidate
- 8 bits / 0x04 — secondary candidate
- off / 0x00 — diagnostic only

Run gfsk-netatmo-raw-start before every transmitted frame.
Production profile 27 remains unchanged.
