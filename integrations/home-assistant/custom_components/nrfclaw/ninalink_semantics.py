"""B7.1 — Home Assistant semantic projection for NinaLink B6 models.

Pure Python on purpose: no Home Assistant import is required for host gates.
B7.2 will adapt these descriptors to DeviceInfo/Entity classes.
"""

from __future__ import annotations

from typing import Any, Dict, Iterable, List

HA_MODEL_SCHEMA = 1


def _slug(name: str) -> str:
    return name.lower().replace(" ", "_")


# capability_id -> HA presentation override
_CAPABILITY_MAP: Dict[int, Dict[str, Any]] = {
    0x0001: {
        "name": "Battery voltage",
        "platform": "sensor",
        "device_class": "voltage",
        "native_unit": "V",
        "state_class": "measurement",
    },
    0x0002: {
        "name": "Battery",
        "platform": "sensor",
        "device_class": "battery",
        "native_unit": "%",
        "state_class": "measurement",
    },
    0x0003: {
        "name": "Supply voltage",
        "platform": "sensor",
        "device_class": "voltage",
        "native_unit": "V",
        "state_class": "measurement",
    },
    0x0100: {
        "name": "Temperature",
        "platform": "sensor",
        "device_class": "temperature",
        "native_unit": "°C",
        "state_class": "measurement",
    },
    0x0101: {
        "name": "Humidity",
        "platform": "sensor",
        "device_class": "humidity",
        "native_unit": "%",
        "state_class": "measurement",
    },
    0x0102: {
        "name": "Illuminance",
        "platform": "sensor",
        "device_class": "illuminance",
        "native_unit": "lx",
        "state_class": "measurement",
    },
    0x0103: {
        "name": "Pressure",
        "platform": "sensor",
        "device_class": "pressure",
        "native_unit": "Pa",
        "state_class": "measurement",
    },
    0x0104: {
        "name": "CO2",
        "platform": "sensor",
        "device_class": "carbon_dioxide",
        "native_unit": "ppm",
        "state_class": "measurement",
    },
    0x0105: {
        "name": "TVOC",
        "platform": "sensor",
        "device_class": None,
        "native_unit": "ppb",
        "state_class": "measurement",
    },
    0x0200: {
        "name": "Motion",
        "platform": "binary_sensor",
        "device_class": "motion",
    },
    0x0201: {
        "name": "Tap",
        "platform": "event",
        "event_type": "tap",
        "event_device_class": "button",
    },
    0x0202: {
        "name": "Fall",
        "platform": "event",
        "event_type": "fall",
        "event_device_class": None,
    },
    0x020A: {
        "name": "Vibration alarm",
        "platform": "binary_sensor",
        "device_class": "problem",
    },
    0x0300: {
        "name": "Digital input",
        "platform": "binary_sensor",
        "device_class": None,
    },
    0x0301: {
        "name": "Hall state",
        "platform": "binary_sensor",
        "device_class": None,
    },
    0x0302: {
        "name": "Counter",
        "platform": "sensor",
        "device_class": None,
        "native_unit": "count",
        "state_class": None,
    },
    0x0303: {
        "name": "Position",
        "platform": "sensor",
        "device_class": None,
        "native_unit": None,
        "state_class": None,
    },
    0x0304: {
        "name": "Pulse frequency",
        "platform": "sensor",
        "device_class": "frequency",
        "native_unit": "Hz",
        "state_class": "measurement",
    },
    0x0400: {
        "name": "Presence",
        "platform": "binary_sensor",
        "device_class": "occupancy",
    },
    0x0401: {
        "name": "Tracking",
        "platform": "binary_sensor",
        "device_class": None,
    },
    0x0500: {
        "name": "Voltage",
        "platform": "sensor",
        "device_class": "voltage",
        "native_unit": "V",
        "state_class": "measurement",
    },
    0x0501: {
        "name": "Current",
        "platform": "sensor",
        "device_class": "current",
        "native_unit": "A",
        "state_class": "measurement",
    },
    0x0502: {
        "name": "Power",
        "platform": "sensor",
        "device_class": "power",
        "native_unit": "W",
        "state_class": "measurement",
    },
    0x0503: {
        "name": "Energy",
        "platform": "sensor",
        "device_class": "energy",
        "native_unit": "Wh",
        "state_class": "total_increasing",
    },
    0x0504: {
        "name": "Line frequency",
        "platform": "sensor",
        "device_class": "frequency",
        "native_unit": "Hz",
        "state_class": "measurement",
    },
    0x0600: {
        "name": "Flow rate",
        "platform": "sensor",
        "device_class": None,
        "native_unit": "L/min",
        "state_class": "measurement",
    },
    0x0601: {
        "name": "Volume",
        "platform": "sensor",
        "device_class": "volume",
        "native_unit": "L",
        "state_class": None,
    },
    0x0602: {
        "name": "Distance",
        "platform": "sensor",
        "device_class": "distance",
        "native_unit": "m",
        "state_class": "measurement",
    },
    0x0603: {
        "name": "Level",
        "platform": "sensor",
        "device_class": None,
        "native_unit": "%",
        "state_class": "measurement",
    },
    0x0604: {
        "name": "RPM",
        "platform": "sensor",
        "device_class": None,
        "native_unit": "rpm",
        "state_class": "measurement",
    },
    0x0605: {
        "name": "Speed",
        "platform": "sensor",
        "device_class": "speed",
        "native_unit": "m/s",
        "state_class": "measurement",
    },
}


_UNIT_FALLBACK = {
    "BOOLEAN": None,
    "PERCENT": "%",
    "VOLT": "V",
    "AMPERE": "A",
    "WATT": "W",
    "WATT_HOUR": "Wh",
    "CELSIUS": "°C",
    "PASCAL": "Pa",
    "LUX": "lx",
    "PPM": "ppm",
    "PPB": "ppb",
    "MILLI_G": "mg",
    "HERTZ": "Hz",
    "SECOND": "s",
    "METER": "m",
    "METER_PER_SECOND": "m/s",
    "LITER": "L",
    "LITER_PER_MINUTE": "L/min",
    "RPM": "rpm",
    "COUNT": "count",
}


def _fallback_platform(cap: Dict[str, Any]) -> str:
    desc = cap.get("descriptor") or {}
    kind = desc.get("kind")
    value_type = cap.get("value_type", desc.get("value_type"))

    if kind == "EVENT":
        return "event"
    if kind in ("STATE", "STATUS") and value_type == 1:
        return "binary_sensor"
    return "sensor"


def _fallback_name(cap: Dict[str, Any]) -> str:
    name = cap.get("name", "UNKNOWN")
    if name == "UNKNOWN":
        return f"Capability {cap['capability_id']}"
    return name.replace("_", " ").title()


def project_capability(
    node_id: int,
    cap: Dict[str, Any],
) -> Dict[str, Any]:
    cap_id = int(cap["capability_id_raw"])
    channel = int(cap["channel"])
    override = dict(_CAPABILITY_MAP.get(cap_id, {}))
    platform = override.pop("platform", _fallback_platform(cap))
    name = override.pop("name", _fallback_name(cap))
    desc = cap.get("descriptor") or {}

    out = {
        "unique_key": f"{node_id:08X}:{cap_id:04X}:{channel}",
        "platform": platform,
        "name": name,
        "capability_id": cap["capability_id"],
        "capability_id_raw": cap_id,
        "channel": channel,
        "value": cap.get("value"),
        "available": True,
        "entity_category": None,
        "device_class": override.pop("device_class", None),
        "native_unit": override.pop(
            "native_unit",
            _UNIT_FALLBACK.get(desc.get("unit")),
        ),
        "state_class": override.pop("state_class", None),
        "descriptor_kind": desc.get("kind"),
        "descriptor_known": bool(desc.get("known")),
    }

    if platform == "event":
        out["event_type"] = override.pop(
            "event_type",
            _slug(cap.get("name", "event")),
        )
        out["event_device_class"] = override.pop(
            "event_device_class", None
        )
        out["event_types"] = [out["event_type"]]
        out["native_unit"] = None
        out["state_class"] = None
        out["value"] = None

    out.update(override)
    return out


def event_as_capability(event: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "capability_id": event["capability_id"],
        "capability_id_raw": event["capability_id_raw"],
        "channel": event["channel"],
        "name": event["name"],
        "value_type": event["value_type"],
        "value": event.get("value"),
        "descriptor": event.get("descriptor") or {},
    }


def project_model(
    model: Dict[str, Any],
    bridge_key: str,
    observed_events: Iterable[Dict[str, Any]] = (),
) -> Dict[str, Any]:
    observed_by_node: Dict[int, Dict[tuple[int, int], Dict[str, Any]]] = {}
    for event in observed_events:
        node_id = int(event["node_id_raw"])
        key = (int(event["capability_id_raw"]), int(event["channel"]))
        observed_by_node.setdefault(node_id, {})[key] = event

    devices: List[Dict[str, Any]] = []
    for node in model["nodes"]:
        node_id = int(node["node_id_raw"])
        entities: Dict[tuple[int, int], Dict[str, Any]] = {}

        for cap in node["capabilities"]:
            key = (int(cap["capability_id_raw"]), int(cap["channel"]))
            entities[key] = project_capability(node_id, cap)

        for key, event in observed_by_node.get(node_id, {}).items():
            if key not in entities:
                entities[key] = project_capability(
                    node_id, event_as_capability(event)
                )

        ordered = [
            entities[key]
            for key in sorted(entities)
        ]

        devices.append({
            "device_key": f"ninalink:{node_id:08X}",
            "node_id": node["node_id"],
            "node_id_raw": node_id,
            "name": f"NinaLink {node_id:08X}",
            "topology": "via_device",
            "via_bridge_key": bridge_key,
            "ready": bool(node.get("ready")),
            "valid": bool(node.get("valid")),
            "session_valid": bool(node.get("session_valid")),
            "entities": ordered,
        })

    devices.sort(key=lambda d: d["node_id_raw"])

    return {
        "ha_model_schema": HA_MODEL_SCHEMA,
        "source_model_schema": model["model_schema"],
        "bridge_key": bridge_key,
        "device_topology": "regular_devices_via_bridge",
        "devices": devices,
    }
