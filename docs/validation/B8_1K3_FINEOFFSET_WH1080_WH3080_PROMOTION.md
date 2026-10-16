# B8.1k3 — Fine Offset WH1080/WH3080 FSK Family Promotion

Profiles:
- 12: 868.300 MHz exact RF variant
- 13: 915.000 MHz exact RF variant

Source decoder: `src/rtl_433/devices/fineoffset_wh1080.c` from the local rtl_433_ESP checkout.

## Source-derived FSK contract

- FSK PCM, 58 us bit width (~17.241 kbps)
- source search anchor `AA 2D D4`
- nRFClaw packet sync `2D D4`; RFVM payload is the 10 bytes after sync
- rtl_433 synthetic `br[0]=0xFF` with CRC init `0xFF` is equivalent to
  CRC-8 poly `0x31`, init `0x00`, zero remainder over the 10 post-sync bytes
- message types: `0xA` weather, `0xB` datetime, `0x7` UV/light
- weather: ID, battery-low flag, signed-magnitude 12-bit temperature,
  humidity, wind average, gust, direction, cumulative rain
- UV/light: UV index and 24-bit light value (`lux = raw * 0.1`)

Datetime telegrams are accepted so automatic ingestion does not report a false
RFVM failure, but B8.1k3 publishes only the common device ID for them. A future
clock semantic can be added separately without changing the RF profile ABI.

The source also computes solar radiation in W/m². B8.1k3 does not publish it
because the current NinaLink unit vocabulary has no W/m² semantic; it is not
silently mislabeled as ordinary power.

## nRFClaw candidate PHY

These are nRFClaw candidate parameters, not rtl_433 source facts:

- deviation: 40 kHz
- LLCC68 RX bandwidth: 117.3 kHz (`0x0B`)
- preamble detector request: 8 bits
- sync: `2D D4`
- fixed post-sync payload: 10 bytes

`HARDWARE_VALIDATED` remains clear for both variants.
