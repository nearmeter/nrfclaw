# B8.1k7d — NetAtmo Runtime Promotion

Profile 27 is runtime-ready using the path proven in B8.1k7c: 868.950 MHz, 117647 bps, +/-50 kHz candidate deviation, RXBW 467 kHz (0x09), 32-bit preamble, receiver discriminator `AA E7 12`, fixed 52-byte receive window, LLCC68 CRC off, and software CRC16/0x8005 init FFFF over LEN+DATA.

Accepted LEN values are 0x06 (STATUS), 0x19 (TH), and 0x31 (WIND). Logical length is LEN+3. Bytes after the logical CRC in short fixed windows are ignored.

TH publishes RF device ID, battery voltage, temperature, and humidity. STATUS for the same full 32-bit TH ID does not erase richer retained TH values. WIND uses its own ID and publishes battery voltage, wind speed in m/s, and direction in degrees.

Controlled CC1101 STATUS/TH/WIND: PASS. Commercial NetAtmo hardware: not tested. HARDWARE_VALIDATED remains false.
