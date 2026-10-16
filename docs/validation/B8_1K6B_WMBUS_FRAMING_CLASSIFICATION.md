# B8.1k6b — Wireless M-Bus framing classification

B8.1k6b is a host/source-analysis gate only.
No LLCC68 runtime behavior is changed.

## Profile 24 — m_bus_mode_c_t

Nominal uplink PHY:

- 868.950 MHz
- 100 kbps
- FSK/NRZ

### Mode C — Format B

Source discriminator: `54 3D 54 3D`.
After the format discriminator the first telegram byte is `L`.
rtl_433 Format-B validation requires `L + 1 <= physical input bytes`.

Status: `PACKET_NATIVE_CANDIDATE`.

### Mode C — Format A

Source discriminator: `54 3D 54 CD`.

Physical frame size is:

    N = ceil((L - 9) / 16)
    physical = L + 3 + 2*N

Status: `BLOCK_CRC_EXPANDS_PHYSICAL_LENGTH`.

### Mode T

Each decoded byte occupies 12 physical bits using 3-of-6 coding.
rtl_433 calls `m_bus_decode_3of6_buffer(...)`.

Status: `3OF6_BITSTREAM_FRAMING`.

## Policy

Profile 24 remains catalog-only because it combines incompatible framing variants.
Profile 25 (Mode-T downlink) remains deferred.

## Next gate

B8.1k6c should implement and validate only Wireless M-Bus Mode C Format B,
868.950 MHz, 100 kbps, as an exact runtime profile rather than declaring
profile 24 entirely runtime-ready.
