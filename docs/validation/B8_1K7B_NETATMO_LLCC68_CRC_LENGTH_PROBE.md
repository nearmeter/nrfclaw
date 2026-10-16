# B8.1k7b — NetAtmo LLCC68 CRC-Length Hardware Probe

Diagnostic-only hardware gate. Profile 27 remains catalog-only.

NetAtmo wire after sync `E7 12`:

    [LEN][DATA x LEN][CRC16]

CRC:
- poly `0x8005`
- init `0xFFFF`
- 2 bytes
- required coverage: `LEN + DATA`

Probe PHY:
- 868.950 MHz
- 117647 bps
- +/-50 kHz synthetic probe deviation
- 467 kHz LLCC68 RX bandwidth
- 32-bit preamble
- variable-length packet
- CRC_2_BYTE

Independent transmitter:
`nrfclaw_cc1101_multi_protocol_simulator_B8_1k7b_netatmo_crc_probe.ino`

Positive test:
1. C8BA09: `gfsk-netatmo-crc-start --bitrate 117647`
2. CC1101: `profile 6`
3. CC1101: `netatmo crc good`
4. CC1101: `send`
5. C8BA09: `gfsk-netatmo-crc-poll`

Expected:
- RX_DONE
- no CRC_ERR
- packet_ready=yes
- reconstructed_len=26
- frame prefix `19 01 5A 91 02 7D AD 57 0D`

Negative test:
1. `gfsk-stop`
2. arm probe again
3. CC1101: `netatmo crc bad`
4. `send`
5. poll

Expected:
- CRC_ERR
- packet_ready=no

Only GOOD=>accepted and BAD=>rejected proves LLCC68 CRC compatibility.
HARDWARE_VALIDATED remains false until a commercial NetAtmo device is tested.
