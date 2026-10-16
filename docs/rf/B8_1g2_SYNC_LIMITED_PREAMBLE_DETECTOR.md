# B8.1g2 — Sync-Limited GFSK Preamble Detector

The WH51 synthetic OTA gate produced `RX_DONE` and the correct 14-byte length,
but a deterministic incorrect FIFO. Changing only RX bandwidth from 93.8 kHz
to 117.3 kHz produced exactly the same incorrect bytes, so bandwidth is not the
primary cause.

The generic nRFClaw GFSK packet helper still programmed a fixed 24-bit preamble
detector:

```c
p[2] = GFSK_PREAMBLE_DETECT_24;
```

That is correct for VEVOR because its sync word is 24 bits (`CA CA 54`), but
WH51 uses a 16-bit sync word (`2D D4`).

For SX126x/LLCC68 GFSK, the preamble detector must be limited by the sync-word
length. B8.1g2 derives:

```text
limit = min(preamble_bits, sync_len * 8)

>=32 -> DETECT_32 / 0x07
>=24 -> DETECT_24 / 0x06
>=16 -> DETECT_16 / 0x05
>= 8 -> DETECT_8  / 0x04
else -> OFF       / 0x00
```

Therefore:

```text
VEVOR: 24-bit preamble, 24-bit sync -> detector 24 / 0x06
WH51:  24-bit preamble, 16-bit sync -> detector 16 / 0x05
WH31E: sync 16 bits                  -> detector <=16 / 0x05
```

This revision changes no profile frequency, bitrate, deviation, bandwidth,
sync, packet length, CRC or whitening setting.

For the cleanest A/B test, keep WH51 at the current 117.3 kHz receiver setting,
flash the receiver only, and repeat the exact same TX frame. If it passes, use
B8.1g1 `--mode primary` afterward to independently test 93.8 kHz with the
detector fix retained.
