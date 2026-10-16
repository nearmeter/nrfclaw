# B8.1k — Candidate Family Promotion: LaCrosse View

Profiles promoted from B8.1j CATALOG-only to runtime candidate:

- profile 19: LaCrosse View LTV-TH
- profile 23: LaCrosse View LTV-WR1
- profile 18: LaCrosse View LTV-WSDTH01 Breeze Pro

Source-derived facts from the pinned/local rtl_433_ESP checkout:

- FSK PCM / NRZ family
- sync/preamble search pattern `D2 AA 2D D4`
- LTV-TH: 8-byte payload, CRC-8 poly `0x31`, init `0x00/0xAC/0xB2`
- LTV-WR1: 11-byte payload, CRC-8 poly `0x31`, init `0x00`
- BreezePro: 11-byte payload, CRC-8 poly `0x31`, init `0x00`
- LTV-TH and LTV-WR1 use ~104 us bits; BreezePro ~107 us

nRFClaw candidate PHY (not asserted by rtl_433 and not hardware validated):

- deviation: 40 kHz
- LLCC68 RX bandwidth: 117.3 kHz (`0x0B`)
- preamble detector request: 32 bits
- exact center frequencies remain those from B8.1j metadata

HA capabilities:

- LTV-TH: device ID, battery OK, temperature, humidity
- LTV-WR1: device ID, wind speed, wind direction
- BreezePro: device ID, temperature, humidity, wind speed, wind direction

WR1 `rain1` / `rain2` are intentionally **not** exposed as rainfall because
rtl_433 labels them raw and states their physical meaning is not yet known.

`HARDWARE_VALIDATED` remains clear until commercial sensors are received.
