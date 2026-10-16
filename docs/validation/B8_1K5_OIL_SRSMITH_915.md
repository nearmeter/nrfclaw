# B8.1k5 — Oil STANDARD + SRSmith 915 MHz

Result:

- profile 30 SRSmith SRS-2C-TX: runtime promoted
- profile 28 Oil Ultrasonic STANDARD FSK: catalog-only,
  blocked by Manchester bitstream framing

SRSmith source: `src/rtl_433/devices/srsmith_pool_srs_2c_tx.c`

SRSmith source contract:
- 915 MHz
- 10 kbps FSK PCM
- 32-bit alternating preamble
- sync `D3 91 D3 91`
- fixed 10-byte payload
- ID = `reverse8(~byte5)`
- button code = byte6
- source acceptance uses CRC-16 poly `0x8005`, init `0xFFFF`,
  bytes 0..7 versus bytes 8..9
- byte7 CRC8 is diagnostic in rtl_433 and is not an acceptance condition

nRFClaw SRSmith candidate PHY:
- deviation 40 kHz
- RXBW 117.3 kHz / `0x0B`
- HW validation remains false

Capabilities:
- `0x0900` RF device ID
- `0x093A` Last button code, retained diagnostic STATUS

`0x093A` is intentionally not an EVENT. Current RF virtual publication writes
provider values to the NinaLink state cache. A later RF CAP_EVENT gate can add
a separate ephemeral event semantic without mutating this frozen status ID.

Oil source: `src/rtl_433/devices/oil_standard.c`

Oil remains catalog-only because rtl_433:
- receives a 2 kbps Manchester half-bit stream
- searches both `55 5D` and `55 62`
- starts Manchester decode at `bitpos+14`
- therefore consumes two bits that lie inside the 16-bit search pattern
- while current LLCC68 profile ABI exposes only byte-aligned data after one
  complete sync word

Independent source examples decode to:
- `94 13 48 54` -> ID `0x9413`, depth 84 cm
- `4E 80 38 21` -> ID `0x4E80`, depth 33 cm

The Oil profile stays RFVM/RADIO/CAPS/HW/RUNTIME = no/no/no/no/no.
A future Manchester bitstream layer should promote it. Depth can then map to
standard `distance` 0x0602; tank percentage must not be invented because tank
geometry is absent from the source protocol.

No CC1101 transmission gate is required.
