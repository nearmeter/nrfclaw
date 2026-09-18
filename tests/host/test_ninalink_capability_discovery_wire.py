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
    "CAPS_REQUEST_LINKED_PAGE0",
    "4e 01 01 02 00 00 03 23 d4 64 ad 01 00 01 00 00 6e 2a",
    18, 0x02,
)

check(
    "CAPS_RESPONSE_TRACKING_WRITABLE",
    "4e 01 00 03 00 00 0d 23 d4 64 ad 01 00 "
    "01 00 01 00 "
    "01 04 00 06 01 00 01 1b 03 "
    "e8 b7",
    28, 0x03,
)

print("B4.10 capability discovery wire vectors: 2/2 PASS")
