#!/usr/bin/env python3
import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NRFCLAW_CLI"))

from nrfclaw_external import NinaLinkExternalModelBuilder

BRIDGE_OPCODE = 0x5A
NODE = 0xAD64D423

class MockApp:
    async def ndp_command(self, opcode, payload):
        assert opcode == BRIDGE_OPCODE
        selector = payload[0]

        if selector == 30:
            return bytes([1, 1, 2, 1]) + (7).to_bytes(4, "little") + (9).to_bytes(4, "little")

        if selector == 31:
            flags = 0x0F
            return (
                NODE.to_bytes(4, "little")
                + bytes([2, 1, 4])
                + (3).to_bytes(2, "little")
                + int(-68).to_bytes(2, "little", signed=True)
                + int(50).to_bytes(2, "little", signed=True)
                + bytes([flags, 0x10])
            )

        if selector == 32:
            index = payload[5]
            if index == 0:
                return (
                    (0x0001).to_bytes(2, "little") + bytes([0, 6])
                    + (3000).to_bytes(4, "little")
                    + (10).to_bytes(2, "little")
                    + (2).to_bytes(2, "little")
                    + (4).to_bytes(2, "little")
                    + bytes([0x10])
                )
            if index == 1:
                return (
                    (0x0100).to_bytes(2, "little") + bytes([0, 5])
                    + (2181).to_bytes(4, "little")
                    + (11).to_bytes(2, "little")
                    + (1).to_bytes(2, "little")
                    + (2).to_bytes(2, "little")
                    + bytes([0x10])
                )
            raise AssertionError(index)

        if selector == 35:
            cap = int.from_bytes(payload[1:3], "little")
            if cap == 0x0001:
                return bytes([1, 1, 6, 0xFD, 3, 0])
            if cap == 0x0100:
                return bytes([1, 1, 5, 0xFE, 7, 0])
            raise AssertionError(hex(cap))

        raise AssertionError(f"unexpected selector {selector}")

async def main():
    builder = NinaLinkExternalModelBuilder(MockApp(), BRIDGE_OPCODE)
    a = await builder.read()
    b = await builder.read()

    assert a == b
    assert a["model_schema"] == 1
    assert a["external_schema"] == 1
    assert a["event_cursor"] == 9

    node = a["nodes"][0]
    assert node["node_id"] == "0xAD64D423"
    assert node["ready"] is True
    assert node["rssi_dbm"] == -34.0
    assert node["snr_db"] == 12.5

    caps = node["capabilities"]
    assert [c["capability_id"] for c in caps] == ["0x0001", "0x0100"]
    assert len({c["key"] for c in caps}) == 2
    assert abs(caps[0]["value"] - 3.0) < 1e-9
    assert abs(caps[1]["value"] - 21.81) < 1e-9

    encoded = json.dumps(a, sort_keys=True, separators=(",", ":"))
    assert json.loads(encoded) == a

    print("PASS deterministic one-shot model")
    print("PASS B5.4 schema -> B6 model schema")
    print("PASS stable node/capability ordering")
    print("PASS unique (node, capability, channel) keys")
    print("PASS descriptor scale/unit projection")
    print("PASS event_cursor initialized from newest_event_id")
    print("PASS idempotent rebuild from identical snapshot")
    print("B6.1a host model gate: PASS")

if __name__ == "__main__":
    asyncio.run(main())
