# B8.1k7d6 — NetAtmo Transition-Density / Long-Run Isolation

Purpose: test whether the intermittent profile-27 corruption is related to
payload transition density / clock recovery rather than carrier-frequency
centering.

B8.1k7d5 repeated +10 kHz and 0 Hz at 27/30 each, so frequency offset was not
the primary cause.

This gate changes only the synthetic TX diagnostic on profile 27. All three
frames are exactly 52 bytes, retain LEN=0x31 and ID=0x015A9103, and have a
valid NetAtmo CRC16/0x8005 init FFFF over LEN+DATA:

- canonical: current WIND vector, CRC 0x0B2E
- transition: bytes 5..49 alternate 0x55/0xAA, CRC 0x04E7
- zero-run: bytes 5..49 are 0x00, CRC 0x1382

Frozen receiver settings:
- RX center 868.950 MHz / offset 0
- bitrate 117647 bps
- deviation +/-50 kHz
- RXBW 373.6 kHz
- preamble detector 24 bits
- fixed 52-byte window
- TX power -9 dBm

Production profile 27 is unchanged. HARDWARE_VALIDATED remains false.
