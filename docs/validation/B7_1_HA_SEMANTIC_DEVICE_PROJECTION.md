# B7.1 — Home Assistant Semantic Device/Entity Projection

## Current Home Assistant topology rule

Home Assistant Core 2026.9 has a first-class child-device model. It is intended
for a lightweight logical part of a parent product and does not carry its own
manufacturer/model/firmware metadata.

A NinaLink remote node is different: it is an independent physical device whose
traffic is routed through the NINASENSE bridge. Therefore B7 models it as a
regular Home Assistant device whose `via_device_id` is the bridge device.

    Home Assistant
         |
         +-- NINASENSE Bridge       regular device
               |
               +-- NinaLink node A  regular device, via bridge
               |
               +-- NinaLink node B  regular device, via bridge

ChildDeviceInfo is reserved for future logical subcomponents of one physical
device, not for NinaLink radio nodes.

## Entity projection

The stable identity of an entity is:

    (node_id, capability_id, channel)

State/value kinds project as follows:

- MEASUREMENT -> sensor
- COUNTER -> sensor
- POSITION -> sensor
- STATE BOOL -> binary_sensor
- STATUS BOOL -> binary_sensor
- EVENT -> event
- unknown/custom -> generic sensor unless descriptor semantics say otherwise

Known standard capabilities refine the Home Assistant device class and unit.
Examples:

- TEMPERATURE -> sensor / temperature / °C
- MOTION -> binary_sensor / motion
- TAP -> event / tap
- BATTERY_VOLTAGE -> sensor / voltage / V
- POWER -> sensor / power / W
- ENERGY -> sensor / energy / Wh

Unknown capability IDs are never silently reinterpreted.

## B5.4 inventory limitation

B5.4 enumerates current state values, but it does not enumerate the complete
per-node capability registry learned by B5.2/B4.10.

Consequences:

- state entities can be projected immediately;
- event capabilities already present in the event journal can be projected;
- an EVENT capability which has never fired cannot yet be pre-created.

B7.2 should close this gap before final Home Assistant runtime integration,
preferably with a read-only external capability-inventory facade.

## CLI preview

    python3 nrfclaw_cli.py --device C8BA09 \
      ninalink-ha-preview --json

This uses Application BLE/NDP and does not require P0.21/NUS.
