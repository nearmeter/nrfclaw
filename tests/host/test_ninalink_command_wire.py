#!/usr/bin/env python3
def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def check(name, h, n):
    b = bytes.fromhex(h)
    assert len(b) == n
    assert int.from_bytes(b[-2:], "little") == crc16(b[:-2])
    print(f"{name}: PASS len={n}")

check("COMMAND_ECHO_U32",
      "4e 01 01 31 00 00 09 23 d4 64 ad 01 00 01 00 01 00 04 78 56 34 12 e8 f7",
      24)
check("COMMAND_RESULT_ECHO_U32",
      "4e 01 00 32 00 00 08 23 d4 64 ad 01 00 01 00 00 04 78 56 34 12 31 f1",
      23)
print("B4.7 COMMAND wire vectors: 2/2 PASS")
