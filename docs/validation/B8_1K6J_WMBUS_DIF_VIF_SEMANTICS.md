# B8.1k6j — Wireless M-Bus DIF/VIF Semantic Decode

Profile 37 keeps the validated B8.1k6c Mode-C / Format-B PHY and CRC path.

Supported controlled application records:
- `0B 13 <BCD3>` -> Volume in whole litres -> `0x0601`
- `02 66 <S16LE>` -> Temperature in 0.1 C -> `0x0100`

`0x0900` RF device ID remains mandatory. Volume and Temperature are optional,
so bare/volume/full simulator variants all remain schema-valid.

Unknown DIF/VIF records are not guessed: semantic parsing stops while the
CRC-valid telegram and fields already decoded remain accepted.

Volume reuses canonical `VOLUME/U32/scale=-3/L`; whole litres are converted to
millilitre raw units. Temperature reuses canonical `TEMPERATURE/S16/scale=-2/C`.

`HARDWARE_VALIDATED` remains false pending a real commercial W-MBus meter.
