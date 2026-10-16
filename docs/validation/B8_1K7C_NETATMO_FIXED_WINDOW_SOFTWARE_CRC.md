# B8.1k7c — NetAtmo Fixed-Window / Software CRC Probe

B8.1k7b hardware rejected both the real NetAtmo CRC (`69 3C`, covering
LEN+DATA) and the diagnostic DATA-only CRC (`6E A3`). Both ended with
`RX_DONE + CRC_ERR = 0x0042`.

B8.1k7c tests a one-profile fallback without using the LLCC68 CRC engine:

- 868.950 MHz
- 117647 bps probe bitrate
- +/-50 kHz probe deviation
- 467 kHz RX bandwidth
- sync E7 12
- FIXED packet mode
- raw RX window = 52 bytes
- LLCC68 CRC = OFF
- logical frame = first `LEN+3` bytes
- software CRC16 poly 0x8005, init 0xFFFF over LEN+DATA

Controlled vectors:

- STATUS: LEN=6, logical=9, CRC=E52A, ID=015A9102
- TH: LEN=25, logical=28, CRC=693C, ID=015A9102
- WIND: LEN=49, logical=52, corrected synthetic CRC=0B2E, ID=015A9103

The WIND vector is the positive fixed-window control because it fills the
complete 52-byte window. STATUS and TH deliberately transmit only their real
9/28 bytes. They prove or disprove whether a single 52-byte fixed receive
profile can preserve shorter NetAtmo telegrams after the transmitter stops.

Profile 27 remains catalog-only. HARDWARE_VALIDATED remains false.
