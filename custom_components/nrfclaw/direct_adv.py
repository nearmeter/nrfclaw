"Direct Advertising Telemetry/Event codec (B7.6d/e v1 + B7.6f v2)."

from __future__ import annotations

from dataclasses import dataclass

MANUFACTURER_ID = 0xFFFF
MAGIC = b"NC"
VERSION_V1 = 1
VERSION_V2 = 2
PAGE_CORE = 0
PAGE_EVENT = 1
PAGE_ACCEL = 2
PAGE_SEMANTIC = 3
PAYLOAD_LEN_V1 = 18
PAYLOAD_LEN_V2 = 19

HA_ROLE_NONE = 0
HA_ROLE_DIRECT_BLE = 1
HA_ROLE_NINALINK_NODE = 2
HA_ROLE_BRIDGE = 3

FLAG_BATTERY_VALID = 0x01
FLAG_TEMPERATURE_VALID = 0x02
FLAG_HALL_VALID = 0x04
FLAG_ACCEL_VALID = 0x01

EVENT_TYPE_NAMES = {
    1: "hall",
    2: "motion",
    3: "tap",
    4: "fall",
    5: "walk",
    6: "vibration_warning",
    7: "vibration_alarm",
    8: "machine_on",
    9: "machine_off",
    10: "vibration_learn_complete",
}
DIRECT_EVENT_TYPES = tuple(EVENT_TYPE_NAMES.values())


@dataclass(slots=True, frozen=True)
class DirectAdvTelemetry:
    sequence: int
    page: int
    role: int
    flags: int
    active_capabilities: int
    battery_v: float | None
    temperature_c: float | None
    hall_mode: int
    hall_channel: int
    hall_value: int | None


@dataclass(slots=True, frozen=True)
class DirectAdvAcceleration:
    sequence: int
    page: int
    role: int
    flags: int
    active_capabilities: int
    accel_valid: bool
    x_mg: int | None
    y_mg: int | None
    z_mg: int | None


@dataclass(slots=True, frozen=True)
class DirectAdvSemantic:
    sequence: int
    page: int
    role: int
    kind: int
    capability_id: int
    channel: int
    value_type: int
    scale10: int
    unit: int
    raw_value: int
    generation: int

    @property
    def value(self) -> float | int:
        signed = self.value_type in (3, 5, 7)
        bits = {1: 8, 2: 8, 3: 8, 4: 16, 5: 16, 6: 32, 7: 32, 8: 8}.get(self.value_type, 32)
        mask = (1 << bits) - 1
        v = self.raw_value & mask
        if signed and v & (1 << (bits - 1)):
            v -= 1 << bits
        if self.value_type == 1:
            v = 1 if v else 0
        return v * (10 ** self.scale10)


@dataclass(slots=True, frozen=True)
class DirectAdvEvent:
    sequence: int
    page: int
    role: int
    event_type_id: int
    event_type: str
    capability_id: int
    channel: int
    arg0: int
    arg1: int
    event_id: int


def advertised_ha_role(payload: bytes) -> int | None:
    if len(payload) == PAYLOAD_LEN_V2 and payload[:2] == MAGIC and payload[2] == VERSION_V2:
        role = payload[5]
        return role if role in (HA_ROLE_DIRECT_BLE, HA_ROLE_BRIDGE) else None
    if len(payload) == PAYLOAD_LEN_V1 and payload[:2] == MAGIC and payload[2] == VERSION_V1:
        # Legacy B7.6d/e packets had no role and were Direct by definition.
        return HA_ROLE_DIRECT_BLE
    return None


def parse_direct_adv_telemetry(payload: bytes) -> DirectAdvTelemetry | None:
    if payload[:2] != MAGIC or len(payload) not in (PAYLOAD_LEN_V1, PAYLOAD_LEN_V2):
        return None

    if payload[2] == VERSION_V1 and len(payload) == PAYLOAD_LEN_V1:
        if payload[4] != PAGE_CORE:
            return None
        flags = payload[5]
        return DirectAdvTelemetry(
            sequence=payload[3], page=payload[4], role=HA_ROLE_DIRECT_BLE,
            flags=flags,
            active_capabilities=int.from_bytes(payload[6:8], "little"),
            battery_v=int.from_bytes(payload[8:10], "little")/1000.0 if flags & FLAG_BATTERY_VALID else None,
            temperature_c=int.from_bytes(payload[10:12], "little", signed=True)/100.0 if flags & FLAG_TEMPERATURE_VALID else None,
            hall_mode=payload[12], hall_channel=payload[13],
            hall_value=int.from_bytes(payload[14:18], "little", signed=True) if flags & FLAG_HALL_VALID else None,
        )

    if payload[2] != VERSION_V2 or len(payload) != PAYLOAD_LEN_V2 or payload[4] != PAGE_CORE:
        return None
    flags = payload[6]
    return DirectAdvTelemetry(
        sequence=payload[3], page=payload[4], role=payload[5], flags=flags,
        active_capabilities=int.from_bytes(payload[7:9], "little"),
        battery_v=int.from_bytes(payload[9:11], "little")/1000.0 if flags & FLAG_BATTERY_VALID else None,
        temperature_c=int.from_bytes(payload[11:13], "little", signed=True)/100.0 if flags & FLAG_TEMPERATURE_VALID else None,
        hall_mode=payload[13], hall_channel=payload[14],
        hall_value=int.from_bytes(payload[15:19], "little", signed=True) if flags & FLAG_HALL_VALID else None,
    )


def parse_direct_adv_event(payload: bytes) -> DirectAdvEvent | None:
    if payload[:2] != MAGIC or len(payload) not in (PAYLOAD_LEN_V1, PAYLOAD_LEN_V2):
        return None

    if payload[2] == VERSION_V1 and len(payload) == PAYLOAD_LEN_V1:
        if payload[4] != PAGE_EVENT:
            return None
        role=HA_ROLE_DIRECT_BLE; event_id=payload[5]; cap=payload[6]; channel=payload[7]
        arg0=int.from_bytes(payload[8:12],"little"); arg1=int.from_bytes(payload[12:16],"little"); eid=int.from_bytes(payload[16:18],"little")
    elif payload[2] == VERSION_V2 and len(payload) == PAYLOAD_LEN_V2:
        if payload[4] != PAGE_EVENT:
            return None
        role=payload[5]; event_id=payload[6]; cap=payload[7]; channel=payload[8]
        arg0=int.from_bytes(payload[9:13],"little"); arg1=int.from_bytes(payload[13:17],"little"); eid=int.from_bytes(payload[17:19],"little")
    else:
        return None

    event_type = EVENT_TYPE_NAMES.get(event_id)
    if event_type is None:
        return None
    return DirectAdvEvent(
        sequence=payload[3], page=payload[4], role=role,
        event_type_id=event_id, event_type=event_type,
        capability_id=cap, channel=channel, arg0=arg0, arg1=arg1, event_id=eid,
    )

def parse_direct_adv_acceleration(payload: bytes) -> DirectAdvAcceleration | None:
    if (
        len(payload) != PAYLOAD_LEN_V2
        or payload[:2] != MAGIC
        or payload[2] != VERSION_V2
        or payload[4] != PAGE_ACCEL
    ):
        return None

    flags = payload[6]
    valid = bool(flags & FLAG_ACCEL_VALID)
    return DirectAdvAcceleration(
        sequence=payload[3],
        page=payload[4],
        role=payload[5],
        flags=flags,
        active_capabilities=int.from_bytes(payload[7:9], "little"),
        accel_valid=valid,
        x_mg=int.from_bytes(payload[9:11], "little", signed=True) if valid else None,
        y_mg=int.from_bytes(payload[11:13], "little", signed=True) if valid else None,
        z_mg=int.from_bytes(payload[13:15], "little", signed=True) if valid else None,
    )



def parse_direct_adv_semantic(payload: bytes) -> DirectAdvSemantic | None:
    if (len(payload) != PAYLOAD_LEN_V2 or payload[:2] != MAGIC or
        payload[2] != VERSION_V2 or payload[4] != PAGE_SEMANTIC):
        return None
    return DirectAdvSemantic(
        sequence=payload[3], page=payload[4], role=payload[5], kind=payload[6],
        capability_id=int.from_bytes(payload[7:9], "little"), channel=payload[9],
        value_type=payload[10], scale10=int.from_bytes(payload[11:12], "little", signed=True),
        unit=payload[12], raw_value=int.from_bytes(payload[13:17], "little"),
        generation=int.from_bytes(payload[17:19], "little"),
    )

def parse_semantic_state_record(payload: bytes) -> DirectAdvSemantic | None:
    if len(payload) != 13:
        return None
    packet = bytes((0x4E,0x43,VERSION_V2,0,PAGE_SEMANTIC,HA_ROLE_DIRECT_BLE)) + payload
    return parse_direct_adv_semantic(packet)
