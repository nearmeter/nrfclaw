#!/usr/bin/env python3

def crc16_ccitt_false(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def check(name, raw_hex, expected_len, expected_type):
    raw = bytes.fromhex(raw_hex)
    assert len(raw) == expected_len, (name, len(raw), expected_len)
    assert raw[3] == expected_type, (name, hex(raw[3]), hex(expected_type))
    wire_crc = int.from_bytes(raw[-2:], "little")
    calc = crc16_ccitt_false(raw[:-2])
    assert wire_crc == calc, (name, hex(wire_crc), hex(calc))
    print(f"{name}: PASS len={len(raw)} crc=0x{wire_crc:04X}")

check(
    "COMMANDS_REQUEST_PAGE0",
    "4e 01 01 33 00 00 03 23 d4 64 ad 01 00 "
    "01 00 00 d1 54",
    18,
    0x33,
)

check(
    "COMMANDS_RESPONSE_PAGE0",
    "4e 01 00 34 00 00 16 23 d4 64 ad 01 00 "
    "01 00 03 03 "
    "01 00 04 04 04 01 "
    "02 00 00 00 08 01 "
    "03 00 00 00 01 01 "
    "26 7a",
    37,
    0x34,
)

print("B4.9 command discovery wire vectors: 2/2 PASS")
