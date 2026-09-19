"""B6.1a — one-shot NinaLink external model over Application BLE/NDP."""

from __future__ import annotations

import struct
from typing import Any, Dict, List

MODEL_SCHEMA = 1

_DISCOVERY_STATES = {
    0: "NEW", 1: "CAPS_PENDING", 2: "CAPS_DONE",
    3: "COMMANDS_PENDING", 4: "READY", 5: "ERROR",
}

_KIND_NAMES = {
    1: "MEASUREMENT", 2: "STATE", 3: "EVENT",
    4: "COUNTER", 5: "POSITION", 6: "STATUS",
}

_UNIT_NAMES = {
    0: "NONE", 1: "BOOLEAN", 2: "PERCENT", 3: "VOLT", 4: "AMPERE",
    5: "WATT", 6: "WATT_HOUR", 7: "CELSIUS", 8: "PASCAL", 9: "LUX",
    10: "PPM", 11: "PPB", 12: "MILLI_G", 13: "HERTZ", 14: "SECOND",
    15: "METER", 16: "METER_PER_SECOND", 17: "LITER",
    18: "LITER_PER_MINUTE", 19: "RPM", 20: "COUNT",
}

_CAPABILITY_NAMES = {
    0x0001: "BATTERY_VOLTAGE", 0x0002: "BATTERY_PERCENT",
    0x0003: "SUPPLY_VOLTAGE", 0x0100: "TEMPERATURE",
    0x0101: "HUMIDITY", 0x0102: "ILLUMINANCE", 0x0103: "PRESSURE",
    0x0104: "CO2", 0x0105: "TVOC", 0x0106: "LEAK",
    0x0200: "MOTION", 0x0201: "TAP", 0x0202: "FALL",
    0x0203: "ACCELERATION_X", 0x0204: "ACCELERATION_Y",
    0x0205: "ACCELERATION_Z", 0x0206: "VIBRATION_RMS",
    0x0207: "VIBRATION_PEAK", 0x0208: "VIBRATION_P2P",
    0x0209: "VIBRATION_FREQUENCY", 0x020A: "VIBRATION_ALARM",
    0x020B: "WALK", 0x0300: "DIGITAL_INPUT", 0x0301: "HALL_STATE",
    0x0302: "COUNTER", 0x0303: "QUADRATURE_POSITION",
    0x0304: "PULSE_FREQUENCY", 0x0400: "PRESENCE",
    0x0401: "TRACKING_ACTIVE", 0x0500: "VOLTAGE",
    0x0501: "CURRENT", 0x0502: "POWER", 0x0503: "ENERGY",
    0x0504: "LINE_FREQUENCY", 0x0600: "FLOW_RATE", 0x0601: "VOLUME",
    0x0602: "DISTANCE", 0x0603: "LEVEL_PERCENT", 0x0604: "RPM",
    0x0605: "SPEED",
}

def _signed(raw: int, bits: int) -> int:
    mask = (1 << bits) - 1
    raw &= mask
    sign = 1 << (bits - 1)
    return raw - (1 << bits) if raw & sign else raw

def typed_value(value_type: int, raw: int) -> Any:
    if value_type == 1:
        return bool(raw & 1)
    if value_type == 2:
        return raw & 0xFF
    if value_type == 3:
        return _signed(raw, 8)
    if value_type == 4:
        return raw & 0xFFFF
    if value_type == 5:
        return _signed(raw, 16)
    if value_type == 6:
        return raw & 0xFFFFFFFF
    if value_type == 7:
        return _signed(raw, 32)
    if value_type == 8:
        return raw & 0xFF
    return raw & 0xFFFFFFFF

def capability_name(capability_id: int) -> str:
    return _CAPABILITY_NAMES.get(capability_id, "UNKNOWN")

class NinaLinkExternalModelBuilder:
    def __init__(self, app: Any, bridge_opcode: int):
        self.app = app
        self.bridge_opcode = bridge_opcode

    async def _command(self, payload: bytes) -> bytes:
        return await self.app.ndp_command(self.bridge_opcode, payload)

    async def _info(self) -> Dict[str, int]:
        p = await self._command(bytes([30]))
        if len(p) != 12:
            raise RuntimeError(f"Invalid B5.4 external INFO length: {len(p)}")
        return {
            "schema": p[0],
            "node_count": p[1],
            "state_value_count": p[2],
            "event_count": p[3],
            "oldest_event_id": int.from_bytes(p[4:8], "little"),
            "newest_event_id": int.from_bytes(p[8:12], "little"),
        }

    async def _descriptor(self, capability_id: int, channel: int) -> Dict[str, Any]:
        p = await self._command(
            bytes([35]) + struct.pack("<H", capability_id) + bytes([channel])
        )
        if len(p) != 6:
            raise RuntimeError(f"Invalid B5.4 descriptor length: {len(p)}")
        scale10 = p[3] - 256 if p[3] & 0x80 else p[3]
        return {
            "known": bool(p[0]),
            "kind_code": p[1],
            "kind": _KIND_NAMES.get(p[1], f"KIND_{p[1]}"),
            "value_type": p[2],
            "scale10": scale10,
            "unit_code": p[4],
            "unit": _UNIT_NAMES.get(p[4], f"UNIT_{p[4]}"),
            "behavior_flags": p[5],
        }

    @staticmethod
    def _engineering_value(value_type: int, value: Any, desc: Dict[str, Any]) -> Any:
        if not desc["known"]:
            return None
        if value_type in (1, 8):
            return value
        if not isinstance(value, (int, float)):
            return None
        return value * (10 ** desc["scale10"])

    async def read(self) -> Dict[str, Any]:
        info = await self._info()
        if info["schema"] != 1:
            raise RuntimeError(
                f"Unsupported B5.4 external schema {info['schema']}; expected 1"
            )

        nodes: List[Dict[str, Any]] = []
        state_total = 0

        for index in range(info["node_count"]):
            p = await self._command(bytes([31, index]))
            if len(p) != 15:
                raise RuntimeError(f"Invalid B5.4 NODE length: {len(p)}")

            node_id = int.from_bytes(p[0:4], "little")
            flags = p[13]
            node = {
                "node_id": f"0x{node_id:08X}",
                "node_id_raw": node_id,
                "state_count": p[4],
                "event_count": p[5],
                "discovery_state": _DISCOVERY_STATES.get(p[6], f"STATE_{p[6]}"),
                "age_s": int.from_bytes(p[7:9], "little"),
                "rssi_dbm": int.from_bytes(p[9:11], "little", signed=True) / 2.0,
                "snr_db": int.from_bytes(p[11:13], "little", signed=True) / 4.0,
                "valid": bool(flags & 0x01),
                "ready": bool(flags & 0x02),
                "has_state": bool(flags & 0x04),
                "session_valid": bool(flags & 0x08),
                "last_message_type": p[14],
                "capabilities": [],
            }

            for state_index in range(node["state_count"]):
                v = await self._command(
                    bytes([32]) + struct.pack("<I", node_id) + bytes([state_index])
                )
                if len(v) != 15:
                    raise RuntimeError(f"Invalid B5.4 STATE length: {len(v)}")

                cap_id = int.from_bytes(v[0:2], "little")
                channel = v[2]
                value_type = v[3]
                raw = int.from_bytes(v[4:8], "little")
                value = typed_value(value_type, raw)
                desc = await self._descriptor(cap_id, channel)
                engineering = self._engineering_value(value_type, value, desc)

                node["capabilities"].append({
                    "key": f"0x{node_id:08X}:0x{cap_id:04X}:{channel}",
                    "capability_id": f"0x{cap_id:04X}",
                    "capability_id_raw": cap_id,
                    "name": capability_name(cap_id),
                    "channel": channel,
                    "value_type": value_type,
                    "raw": raw,
                    "typed_value": value,
                    "engineering_value": engineering,
                    "value": engineering if desc["known"] else value,
                    "value_source": "engineering" if desc["known"] else "typed",
                    "descriptor": desc,
                    "sequence": int.from_bytes(v[8:10], "little"),
                    "age_s": int.from_bytes(v[10:12], "little"),
                    "updates": int.from_bytes(v[12:14], "little"),
                    "source_message_type": v[14],
                })
                state_total += 1

            node["capabilities"].sort(
                key=lambda x: (x["capability_id_raw"], x["channel"])
            )
            nodes.append(node)

        nodes.sort(key=lambda x: x["node_id_raw"])

        if state_total != info["state_value_count"]:
            raise RuntimeError(
                "B6.1a state count changed during snapshot: "
                f"info={info['state_value_count']} enumerated={state_total}"
            )

        return {
            "model_schema": MODEL_SCHEMA,
            "external_schema": info["schema"],
            "source": "NinaLink B5.4",
            "node_count": info["node_count"],
            "state_value_count": info["state_value_count"],
            "event_cursor": info["newest_event_id"],
            "event_window": {
                "count": info["event_count"],
                "oldest_id": info["oldest_event_id"],
                "newest_id": info["newest_event_id"],
            },
            "nodes": nodes,
        }


class NinaLinkExternalReconciler:
    """B6.1b live reconciliation above authoritative B5.4 + B5.5 hints."""

    def __init__(self, app: Any, bridge_opcode: int):
        self.app = app
        self.bridge_opcode = bridge_opcode
        self.builder = NinaLinkExternalModelBuilder(app, bridge_opcode)
        self.model = None
        self.event_cursor = 0
        self.state_revision = 0
        self.event_revision = 0
        self.change_revision = 0

    async def _command(self, payload: bytes) -> bytes:
        return await self.app.ndp_command(self.bridge_opcode, payload)

    async def _read_event_descriptor(
        self, capability_id: int, channel: int
    ) -> Dict[str, Any]:
        return await self.builder._descriptor(capability_id, channel)

    async def read_events(self, cursor: int, limit: int = 64) -> Dict[str, Any]:
        if not 0 <= cursor <= 0xFFFFFFFF:
            raise ValueError("cursor must be 0..0xFFFFFFFF")
        if not 1 <= limit <= 64:
            raise ValueError("limit must be 1..64")

        info = await self.builder._info()
        oldest = info["oldest_event_id"]
        newest = info["newest_event_id"]

        stream_reset = bool(cursor and (newest == 0 or cursor > newest))
        cur = 0 if stream_reset else cursor
        overrun = bool(cur and oldest and (cur + 1) < oldest)
        events = []

        for _ in range(limit):
            p = await self._command(bytes([33]) + struct.pack("<I", cur))
            if len(p) != 15:
                raise RuntimeError(
                    f"Invalid B5.4 EVENT_NEXT_META length: {len(p)}"
                )
            if not p[0]:
                break

            event_id = int.from_bytes(p[1:5], "little")
            node_id = int.from_bytes(p[5:9], "little")
            capability_id = int.from_bytes(p[9:11], "little")
            channel = p[11]
            value_type = p[12]
            sequence = int.from_bytes(p[13:15], "little")

            vp = await self._command(
                bytes([34]) + struct.pack("<I", event_id)
            )
            if len(vp) != 7 or not vp[0]:
                raise RuntimeError(
                    f"B5.4 event {event_id} disappeared during reconciliation"
                )

            raw = int.from_bytes(vp[1:5], "little")
            age_s = int.from_bytes(vp[5:7], "little")
            desc = await self._read_event_descriptor(
                capability_id, channel
            )
            typed = typed_value(value_type, raw)
            engineering = self.builder._engineering_value(
                value_type, typed, desc
            )

            events.append({
                "event_id": event_id,
                "node_id": f"0x{node_id:08X}",
                "node_id_raw": node_id,
                "capability_id": f"0x{capability_id:04X}",
                "capability_id_raw": capability_id,
                "name": capability_name(capability_id),
                "channel": channel,
                "value_type": value_type,
                "raw": raw,
                "typed_value": typed,
                "engineering_value": engineering,
                "value": engineering if desc["known"] else typed,
                "value_source": (
                    "engineering" if desc["known"] else "typed"
                ),
                "descriptor": desc,
                "sequence": sequence,
                "age_s": age_s,
            })
            cur = event_id

        return {
            "requested_cursor": cursor,
            "next_cursor": cur,
            "overrun": overrun,
            "stream_reset": stream_reset,
            "window": {
                "count": info["event_count"],
                "oldest_id": oldest,
                "newest_id": newest,
            },
            "events": events,
        }

    async def initialize(self, subscription: Dict[str, Any]) -> Dict[str, Any]:
        self.model = await self.builder.read()
        self.event_cursor = self.model["event_cursor"]
        self.state_revision = subscription["state_revision"]
        self.event_revision = subscription["event_revision"]
        self.change_revision = subscription.get("change_revision", 0)

        return {
            "type": "initial",
            "state_revision": self.state_revision,
            "event_revision": self.event_revision,
            "change_revision": self.change_revision,
            "event_cursor": self.event_cursor,
            "model": self.model,
        }

    async def reconcile(self, change: Dict[str, Any]) -> Dict[str, Any]:
        if self.model is None:
            raise RuntimeError("reconciler must be initialized first")

        state_refreshed = False
        event_result = {
            "requested_cursor": self.event_cursor,
            "next_cursor": self.event_cursor,
            "overrun": False,
            "stream_reset": False,
            "window": self.model["event_window"],
            "events": [],
        }

        # STATE and EVENT are intentionally reconciled independently.
        # A state refresh must never advance the event cursor.
        if change.get("state_changed"):
            saved_cursor = self.event_cursor
            refreshed = await self.builder.read()
            refreshed["event_cursor"] = saved_cursor
            self.model = refreshed
            state_refreshed = True

        if change.get("event_changed"):
            event_result = await self.read_events(self.event_cursor)
            self.event_cursor = event_result["next_cursor"]
            self.model["event_cursor"] = self.event_cursor
            self.model["event_window"] = event_result["window"]

        self.state_revision = change["state_revision"]
        self.event_revision = change["event_revision"]
        self.change_revision = change["change_revision"]

        return {
            "type": "reconcile",
            "flags": change["flags"],
            "state_changed": bool(change.get("state_changed")),
            "event_changed": bool(change.get("event_changed")),
            "state_refreshed": state_refreshed,
            "state_revision": self.state_revision,
            "event_revision": self.event_revision,
            "change_revision": self.change_revision,
            "event_cursor": self.event_cursor,
            "overrun": event_result["overrun"],
            "stream_reset": event_result["stream_reset"],
            "events": event_result["events"],
            "model": self.model,
        }
