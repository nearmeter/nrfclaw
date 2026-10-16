# B8.1k6k — Wireless M-Bus General DIF/VIF Record Parser

B8.1k6k expands the B8.1k6j fixed-record decoder into a bounded record walker.

External semantics remain frozen:

- `0x0900` RF device ID
- `0x0601` Volume — U32, scale -3, L
- `0x0100` Temperature — S16, scale -2, Celsius

For CI `0x78`, the parser now:

- accepts `0x2F` filler between records
- walks up to 10 DIFE bytes
- walks up to 10 VIFE bytes
- consumes multiple records
- skips unknown but structurally valid records
- rejects structurally truncated records
- projects only instantaneous storage-0/tariff-0/subunit-0 values
- avoids guessing semantic meaning for VIFE-modified records

DIF traversal understands scalar length for:

- signed 8/16/24/32-bit integer
- float32 (skip)
- int48/int64 (skip)
- BCD2/BCD4/BCD6/BCD8
- LVAR (skip)
- BCD12 when representable in the RFVM int32 result

Primary VIF semantic support:

- `0x10..0x17` Volume, normalized to integer millilitres
- `0x64..0x67` External Temperature, normalized to centi-Celsius

Pending by design:

- CI 0x72 long header
- CI 0x7A short header
- encrypted payloads
- extended VIF semantic tables
- multiple history/channel projections
- semantic continuation across Format-B second CRC/data block
