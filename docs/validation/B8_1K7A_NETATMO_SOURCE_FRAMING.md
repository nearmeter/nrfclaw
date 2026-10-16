# B8.1k7a — NetAtmo 868.95 MHz Source/Framing Classification

Profile: **27**

Upstream decoder: `rtl_433/src/devices/netatmo_thw.c`

B8.1k7a is a source/host classification gate only. It does not enable
RFVM/RADIO/CAPABILITY runtime flags.

## Source PHY/framing

- center frequency: 868.950 MHz
- modulation: FSK PCM / NRZ
- decoder timing: 8.5 us short/long pulses
- catalog-derived bitrate: 117647 bps
- source prose also states 97.600 kbit/s; this mismatch is deliberately not
  guessed away in B8.1k7a
- preamble: repeated `AA`
- sync: `E7 12`
- post-sync: `[LEN][DATA x LEN][CRC16 MSB][CRC16 LSB]`
- physical bytes after sync: `LEN + 3`
- CRC16: poly `0x8005`, init `0xFFFF`
- CRC coverage: `LEN + DATA`

## Message classes

- `LEN=6`: TH status
- `LEN=0x19`: temperature/humidity data
- `LEN=0x31`: ultrasonic wind data

Valid TH reference:

- ID `0x015A9102`
- battery `6830 mV`
- temperature `11.8 C`
- humidity `88 %`
- CRC `0x693C`

The documented wind payload computes approximately `3.1674 km/h` and `236 deg`,
but the comment ends in CRC `A152` while the decoder algorithm calculates
`0B2E`. That vector is therefore used only as a payload-math reference, not as
a valid acceptance vector.

## LLCC68 open question

LLCC68 variable packet mode consumes the length byte. NetAtmo's length byte
describes DATA only; the two CRC bytes follow separately. That is structurally
promising for variable mode + hardware CRC.

The unresolved point is CRC coverage:

- NetAtmo requires CRC over `LEN + DATA`.
- LLCC68 documentation treats the packet length as outside the payload.

B8.1k7b must therefore prove on silicon whether custom LLCC68 CRC includes the
variable-length byte in a way compatible with NetAtmo.

If yes, one variable-length NetAtmo profile can cover status/TH/wind.
If no, the fallback is derived fixed-length profiles with software CRC.

`HARDWARE_VALIDATED` remains false.
