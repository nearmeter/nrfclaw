#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(
    ROOT / "integrations/home-assistant/custom_components/nrfclaw"
))

from ninalink_semantics import project_capability, project_model


NODE = 0xAD64D423

def desc(kind, value_type, unit, known=True):
    return {
        "known": known,
        "kind": kind,
        "value_type": value_type,
        "unit": unit,
    }

model = {
    "model_schema": 1,
    "nodes": [{
        "node_id": "0xAD64D423",
        "node_id_raw": NODE,
        "ready": True,
        "valid": True,
        "session_valid": True,
        "capabilities": [
            {
                "capability_id": "0x0100",
                "capability_id_raw": 0x0100,
                "channel": 0,
                "name": "TEMPERATURE",
                "value_type": 5,
                "value": 21.81,
                "descriptor": desc("MEASUREMENT", 5, "CELSIUS"),
            },
            {
                "capability_id": "0x0200",
                "capability_id_raw": 0x0200,
                "channel": 0,
                "name": "MOTION",
                "value_type": 1,
                "value": True,
                "descriptor": desc("STATE", 1, "BOOLEAN"),
            },
            {
                "capability_id": "0x9001",
                "capability_id_raw": 0x9001,
                "channel": 2,
                "name": "UNKNOWN",
                "value_type": 6,
                "value": 123,
                "descriptor": desc("MEASUREMENT", 6, "COUNT", False),
            },
        ],
    }],
}

events = [{
    "node_id": "0xAD64D423",
    "node_id_raw": NODE,
    "capability_id": "0x0201",
    "capability_id_raw": 0x0201,
    "channel": 0,
    "name": "TAP",
    "value_type": 8,
    "value": 2,
    "descriptor": desc("EVENT", 8, "NONE"),
}]

out = project_model(model, "bridge:C8BA09", events)
assert out["device_topology"] == "regular_devices_via_bridge"
assert len(out["devices"]) == 1

device = out["devices"][0]
assert device["device_key"] == "ninalink:AD64D423"
assert device["via_bridge_key"] == "bridge:C8BA09"
assert device["topology"] == "via_device"

entities = {
    e["capability_id"]: e
    for e in device["entities"]
}

temp = entities["0x0100"]
assert temp["platform"] == "sensor"
assert temp["device_class"] == "temperature"
assert temp["native_unit"] == "°C"
assert temp["state_class"] == "measurement"
assert temp["value"] == 21.81

motion = entities["0x0200"]
assert motion["platform"] == "binary_sensor"
assert motion["device_class"] == "motion"
assert motion["value"] is True

tap = entities["0x0201"]
assert tap["platform"] == "event"
assert tap["event_type"] == "tap"
assert tap["event_types"] == ["tap"]
assert tap["value"] is None

unknown = entities["0x9001"]
assert unknown["platform"] == "sensor"
assert unknown["device_class"] is None
assert unknown["value"] == 123

assert len({e["unique_key"] for e in device["entities"]}) == 4

print("PASS NinaLink node projects as regular HA device via bridge")
print("PASS temperature -> sensor/temperature/degC")
print("PASS motion BOOL STATE -> binary_sensor/motion")
print("PASS TAP EVENT -> event entity")
print("PASS unknown capability -> generic sensor without reinterpretation")
print("PASS entity unique keys stable by node/capability/channel")
print("B7.1 HA semantic projection gate: PASS")
