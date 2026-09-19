#!/usr/bin/env python3
import asyncio
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NRFCLAW_CLI"))

from nrfclaw_external import NinaLinkExternalReconciler

OP = 0x5A
NODE = 0xAD64D423


def state_frame(temp_raw, sequence=0, updates=1):
    return (
        (0x0100).to_bytes(2, "little")
        + bytes([0, 5])
        + int(temp_raw).to_bytes(4, "little", signed=False)
        + int(sequence).to_bytes(2, "little")
        + (1).to_bytes(2, "little")
        + int(updates).to_bytes(2, "little")
        + bytes([0x10])
    )


class MockApp:
    def __init__(self):
        self.temp_raw = 2000
        self.event_present = False

    async def ndp_command(self, opcode, payload):
        assert opcode == OP
        sel = payload[0]

        if sel == 30:
            event_count = 1 if self.event_present else 0
            oldest = 5 if self.event_present else 0
            newest = 5 if self.event_present else 0
            return (
                bytes([1, 1, 1, event_count])
                + oldest.to_bytes(4, "little")
                + newest.to_bytes(4, "little")
            )

        if sel == 31:
            return (
                NODE.to_bytes(4, "little")
                + bytes([1, 1 if self.event_present else 0, 4])
                + (1).to_bytes(2, "little")
                + int(-70).to_bytes(2, "little", signed=True)
                + int(48).to_bytes(2, "little", signed=True)
                + bytes([0x0F, 0x10])
            )

        if sel == 32:
            return state_frame(self.temp_raw)

        if sel == 35:
            cap = int.from_bytes(payload[1:3], "little")
            if cap == 0x0100:
                return bytes([1, 1, 5, 0xFE, 7, 0])
            if cap == 0x0201:
                return bytes([1, 3, 8, 0, 0, 0])
            raise AssertionError(hex(cap))

        if sel == 33:
            cursor = int.from_bytes(payload[1:5], "little")
            if self.event_present and cursor < 5:
                return (
                    bytes([1])
                    + (5).to_bytes(4, "little")
                    + NODE.to_bytes(4, "little")
                    + (0x0201).to_bytes(2, "little")
                    + bytes([0, 8])
                    + (1).to_bytes(2, "little")
                )
            return bytes(15)

        if sel == 34:
            event_id = int.from_bytes(payload[1:5], "little")
            assert event_id == 5
            return bytes([1]) + (1).to_bytes(4, "little") + (1).to_bytes(2, "little")

        raise AssertionError(sel)


async def main():
    app = MockApp()
    r = NinaLinkExternalReconciler(app, OP)

    initial = await r.initialize({
        "state_revision": 10,
        "event_revision": 20,
        "change_revision": 30,
    })
    assert initial["event_cursor"] == 0
    assert initial["model"]["nodes"][0]["capabilities"][0]["value"] == 20.0

    # STATE only: model refreshes, cursor MUST NOT advance.
    app.temp_raw = 2181
    state = await r.reconcile({
        "flags": 1,
        "state_changed": True,
        "event_changed": False,
        "state_revision": 11,
        "event_revision": 20,
        "change_revision": 31,
    })
    assert state["state_refreshed"]
    assert state["event_cursor"] == 0
    assert state["events"] == []
    assert state["model"]["nodes"][0]["capabilities"][0]["value"] == 21.81

    # EVENT only: drain after cursor and advance exactly to event 5.
    app.event_present = True
    event = await r.reconcile({
        "flags": 2,
        "state_changed": False,
        "event_changed": True,
        "state_revision": 11,
        "event_revision": 21,
        "change_revision": 32,
    })
    assert not event["state_refreshed"]
    assert event["event_cursor"] == 5
    assert not event["overrun"]
    assert not event["stream_reset"]
    assert len(event["events"]) == 1
    assert event["events"][0]["event_id"] == 5
    assert event["events"][0]["capability_id"] == "0x0201"
    assert event["events"][0]["name"] == "TAP"

    # Reconcile same event revision/hint again: cursor prevents replay.
    repeat = await r.reconcile({
        "flags": 2,
        "state_changed": False,
        "event_changed": True,
        "state_revision": 11,
        "event_revision": 21,
        "change_revision": 33,
    })
    assert repeat["event_cursor"] == 5
    assert repeat["events"] == []

    print("PASS subscribe baseline -> initial B5.4 snapshot")
    print("PASS STATE hint refreshes authoritative snapshot")
    print("PASS STATE reconcile does not advance event cursor")
    print("PASS EVENT hint drains journal after cursor")
    print("PASS event cursor advances monotonically")
    print("PASS repeated EVENT reconcile does not replay event")
    print("PASS state/event reconciliation remains independent")
    print("B6.1b reconciliation gate: PASS")


if __name__ == "__main__":
    asyncio.run(main())
