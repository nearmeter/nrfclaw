# nRFClaw Capability Registry v1

Status: **B1.1 candidate**
Baseline: `b0-pre-ninalink-20260917` / `97c6749ffc50cac769aa17fb097bab9481cf54cf`

## 1. Purpose

This document defines the semantic capability namespace used by NinaLink and, later,
by the nRFClaw Bridge and Home Assistant integration.

The registry describes **what a value means**, not which physical device produced it.

Examples:

- a DS18B20 produces `temperature`;
- a LIS2DH12 can produce `motion`, `tap`, `fall`, acceleration and vibration capabilities;
- a Hall/reed input can produce `digital_input`, `hall_state`, `counter` or
  `quadrature_position`;
- a future SHT31 may produce `temperature` and `humidity`;
- a future BH1750 may produce `illuminance`;
- a future gas sensor may produce a standardized gas/air-quality capability.

Changing the physical sensor must not require changing NinaLink or Home Assistant
semantics when the measured quantity is unchanged.

## 2. B0 compatibility boundary

This registry is a **new namespace**.

It MUST NOT reuse or redefine:

- `NRFCLAW_CAP_*` from `include/nrfclaw_native.h`;
- NDP v1 opcodes from `include/nrfclaw_ndp.h`;
- NDP `SENSOR_READ` selectors;
- VM opcodes or existing pseudo-code primitives;
- `nrfclaw_event_type_t` values.

The existing `NRFCLAW_CAP_*` values describe firmware/board feature support
(GPIO, BATTERY, DS18B20, ACCEL, LORA, BLE_APP, etc.). NinaLink capability IDs
describe semantic measurements, states and events.

Example:

```text
NRFCLAW_CAP_DS18B20 = 3
    means: firmware/board supports the DS18B20 feature

NINALINK_CAP_TEMPERATURE = 0x0100
    means: this endpoint exposes a temperature value
```

The two namespaces are intentionally unrelated.

## 3. Registry version

Registry version: **1**

Rules:

1. A standardized capability ID is never reused for a different meaning.
2. Once published, its physical meaning, default unit and scaling are immutable.
3. A deprecated capability remains reserved forever.
4. New capability IDs may be added without changing the registry version.
5. An incompatible semantic change requires a new ID.
6. Unknown capability IDs MUST NOT cause a NinaLink frame to be rejected solely
   because the receiver does not recognize the capability.
7. Bridges should preserve and forward unknown capability IDs whenever the frame
   itself is valid.

## 4. Capability key

A concrete capability endpoint is identified by:

```text
(capability_id, channel)
```

Where:

- `capability_id` is an unsigned 16-bit registry ID;
- `channel` is an unsigned 8-bit instance number;
- channel `0` is the first/default instance.

Examples:

```text
temperature[0]   room sensor
temperature[1]   evaporator sensor
counter[0]       Hall input 1
counter[1]       Hall input 2
```

Channels identify **instances**, not axes or sub-fields.

For example, X/Y/Z acceleration have distinct capability IDs. A second
accelerometer uses the same X/Y/Z IDs with `channel=1`.

## 5. Capability ID ranges

| Range | Purpose |
|---|---|
| `0x0000` | Invalid / not assigned |
| `0x0001–0x00FF` | Device and system telemetry |
| `0x0100–0x01FF` | Environmental sensing |
| `0x0200–0x02FF` | Motion and mechanical sensing |
| `0x0300–0x03FF` | Digital inputs, Hall and counting |
| `0x0400–0x04FF` | Presence and tracking state |
| `0x0500–0x05FF` | Electrical and energy measurements |
| `0x0600–0x06FF` | Process / industrial measurements |
| `0x0700–0x07FF` | Position / orientation |
| `0x0800–0x7FFF` | Reserved for future standardized categories |
| `0x8000–0xBFFF` | Experimental/vendor capabilities |
| `0xC000–0xFFFE` | Local/private capabilities |
| `0xFFFF` | Invalid / reserved |

Experimental/vendor and local/private IDs are not guaranteed to have globally
stable semantics unless accompanied by a descriptor.

## 6. Capability kinds

Every capability has one semantic kind.

| Kind | Meaning | Example |
|---|---|---|
| `MEASUREMENT` | Sampled physical quantity | temperature, voltage |
| `STATE` | Current boolean/enumerated state | motion, leak, digital input |
| `EVENT` | Occurrence, not a persistent level | tap, fall |
| `COUNTER` | Monotonic or application-controlled count | pulse counter |
| `POSITION` | Signed/absolute position | quadrature position |
| `STATUS` | Runtime/operational status | tracking active |

The numeric wire encoding for kinds is defined by NinaLink v1 in B1.2.

## 7. Value types

B1.1 standardizes the supported semantic value types:

- `BOOL`
- `U8`
- `S8`
- `U16`
- `S16`
- `U32`
- `S32`
- `ENUM8`

Floating-point values are not transported as IEEE-754 in registry v1.

Measurements use fixed-point integers:

```text
engineering_value = raw_value * 10^scale10
```

Example:

```text
temperature:
raw = 1988
scale10 = -2
engineering value = 19.88 °C
```

The numeric wire encoding for value types is defined in B1.2.

## 8. Units

The registry uses semantic unit names. Their compact numeric encoding is defined
in B1.2.

Initial unit set:

- `NONE`
- `BOOLEAN`
- `PERCENT`
- `VOLT`
- `AMPERE`
- `WATT`
- `WATT_HOUR`
- `CELSIUS`
- `PASCAL`
- `LUX`
- `PPM`
- `PPB`
- `MILLI_G`
- `HERTZ`
- `SECOND`
- `METER`
- `METER_PER_SECOND`
- `LITER`
- `LITER_PER_MINUTE`
- `RPM`
- `COUNT`

A capability may define `NONE` for an enum or status whose meaning is defined
by that capability.

## 9. Standard capability registry

### 9.1 Device / system — `0x0001–0x00FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0001` | `battery_voltage` | MEASUREMENT | U16 | -3 | VOLT | 3300 = 3.300 V |
| `0x0002` | `battery_percent` | MEASUREMENT | U8 | 0 | PERCENT | 0..100 |
| `0x0003` | `supply_voltage` | MEASUREMENT | U16 | -3 | VOLT | Non-battery supply rail |

RSSI and SNR are deliberately not node capabilities in v1. They are link
metadata measured by the receiver/bridge and belong to the NinaLink transport
metadata defined in B1.2/B3.

### 9.2 Environmental — `0x0100–0x01FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0100` | `temperature` | MEASUREMENT | S16 | -2 | CELSIUS | 1988 = 19.88 °C |
| `0x0101` | `humidity` | MEASUREMENT | U16 | -2 | PERCENT | 5725 = 57.25 % |
| `0x0102` | `illuminance` | MEASUREMENT | U32 | 0 | LUX | Ambient light |
| `0x0103` | `pressure` | MEASUREMENT | U32 | 0 | PASCAL | Absolute pressure |
| `0x0104` | `co2` | MEASUREMENT | U16 | 0 | PPM | CO2 concentration |
| `0x0105` | `tvoc` | MEASUREMENT | U32 | 0 | PPB | Total VOC |
| `0x0106` | `leak` | STATE | BOOL | 0 | BOOLEAN | Water/liquid detected |

Additional gases MUST receive explicit standardized IDs when their chemical
meaning is known. A generic `gas` ID is intentionally not standardized in v1,
because ppm of CO, CH4, NH3, H2S, etc. are not semantically interchangeable.

### 9.3 Motion / mechanical — `0x0200–0x02FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0200` | `motion` | STATE | BOOL | 0 | BOOLEAN | Motion currently asserted |
| `0x0201` | `tap` | EVENT | ENUM8 | 0 | NONE | Tap event; enum details may be extended |
| `0x0202` | `fall` | EVENT | BOOL | 0 | BOOLEAN | Fall event |
| `0x0203` | `acceleration_x` | MEASUREMENT | S16 | 0 | MILLI_G | X axis |
| `0x0204` | `acceleration_y` | MEASUREMENT | S16 | 0 | MILLI_G | Y axis |
| `0x0205` | `acceleration_z` | MEASUREMENT | S16 | 0 | MILLI_G | Z axis |
| `0x0206` | `vibration_rms` | MEASUREMENT | U16 | 0 | MILLI_G | RMS acceleration |
| `0x0207` | `vibration_peak` | MEASUREMENT | U16 | 0 | MILLI_G | Peak acceleration |
| `0x0208` | `vibration_peak_to_peak` | MEASUREMENT | U16 | 0 | MILLI_G | Peak-to-peak acceleration |
| `0x0209` | `vibration_frequency` | MEASUREMENT | U32 | -3 | HERTZ | 1000 = 1.000 Hz |
| `0x020A` | `vibration_alarm` | STATE | BOOL | 0 | BOOLEAN | VIB_AUTO abnormal state |
| `0x020B` | `walk` | EVENT | BOOL | 0 | BOOLEAN | Walking/activity event |

`vibration_alarm` is a semantic alarm state. It is not the same namespace as
the legacy native firmware capability `NRFCLAW_CAP_VIB_AUTO`.

### 9.4 Digital / Hall / counting — `0x0300–0x03FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0300` | `digital_input` | STATE | BOOL | 0 | BOOLEAN | Generic digital input |
| `0x0301` | `hall_state` | STATE | BOOL | 0 | BOOLEAN | Hall/reed logical state |
| `0x0302` | `counter` | COUNTER | U32 | 0 | COUNT | Pulse/event count |
| `0x0303` | `quadrature_position` | POSITION | S32 | 0 | COUNT | Signed encoder position |
| `0x0304` | `pulse_frequency` | MEASUREMENT | U32 | -3 | HERTZ | Derived pulse frequency |

### 9.5 Presence / tracking — `0x0400–0x04FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0400` | `presence` | STATE | BOOL | 0 | BOOLEAN | Generic occupancy/presence |
| `0x0401` | `tracking_active` | STATUS | BOOL | 0 | BOOLEAN | Tracking runtime active |

`tracking_active` reports the nRFClaw tracking runtime state. It does not imply
that the node itself knows geographic coordinates.

### 9.6 Electrical / energy — `0x0500–0x05FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0500` | `voltage` | MEASUREMENT | S32 | -3 | VOLT | Generic measured voltage |
| `0x0501` | `current` | MEASUREMENT | S32 | -3 | AMPERE | Generic measured current |
| `0x0502` | `power` | MEASUREMENT | S32 | -3 | WATT | Signed power allowed |
| `0x0503` | `energy` | COUNTER | U32 | -3 | WATT_HOUR | Accumulated energy |
| `0x0504` | `line_frequency` | MEASUREMENT | U32 | -3 | HERTZ | AC/system frequency |

### 9.7 Process / industrial — `0x0600–0x06FF`

| ID | Name | Kind | Type | scale10 | Unit | Notes |
|---:|---|---|---|---:|---|---|
| `0x0600` | `flow_rate` | MEASUREMENT | U32 | -3 | LITER_PER_MINUTE | Generic fluid flow |
| `0x0601` | `volume` | COUNTER | U32 | -3 | LITER | Accumulated volume |
| `0x0602` | `distance` | MEASUREMENT | U32 | -3 | METER | Distance/range |
| `0x0603` | `level_percent` | MEASUREMENT | U16 | -2 | PERCENT | Tank/level percentage |
| `0x0604` | `rpm` | MEASUREMENT | U32 | 0 | RPM | Rotational speed |
| `0x0605` | `speed` | MEASUREMENT | S32 | -3 | METER_PER_SECOND | Linear speed |

### 9.8 Position / orientation — `0x0700–0x07FF`

Reserved in B1.1. No standardized IDs are assigned yet.

## 10. Runtime capability state

The registry describes semantic capability identity. Runtime availability is a
separate concept.

B1.1 defines these semantic state bits; their compact wire representation is
defined in B1.2:

- `SUPPORTED` — firmware/board can implement the capability;
- `PRESENT` — required hardware/source is currently detected;
- `ENABLED` — capability is currently configured/running;
- `FAULT` — capability/source exists but is currently faulted.

Examples:

```text
DS18B20 absent:
temperature: SUPPORTED, !PRESENT

DS18B20 detected:
temperature: SUPPORTED | PRESENT | ENABLED

Hall compiled in but disabled:
hall_state: SUPPORTED | PRESENT, !ENABLED

Tracking provisioned but stopped:
tracking_active: SUPPORTED | PRESENT, !ENABLED
```

This preserves the distinction already present in B0 between native capability
support and current runtime activity.

## 11. Capability behavior flags

A capability descriptor may declare behavior flags:

- `READABLE` — current value may be requested;
- `REPORTABLE` — node may publish a value asynchronously;
- `EVENT_SOURCE` — capability may generate asynchronous events;
- `WRITABLE` — future: value/config may be set remotely;
- `RETAINED` — last value remains meaningful until replaced.

These flags describe behavior, not runtime state.

The exact bit encoding is B1.2 work.

## 12. Standard vs custom descriptors

### Standard IDs

For standardized IDs below `0x8000`, the registry defines:

- name;
- kind;
- value type;
- scale;
- unit.

A sender MUST NOT change those semantics.

### Experimental/vendor IDs — `0x8000–0xBFFF`

These may be used for capabilities under development.

A descriptor must provide enough metadata for a receiver to transport and
display the value generically:

- capability ID;
- short name;
- kind;
- value type;
- scale;
- unit;
- behavior flags.

### Local/private IDs — `0xC000–0xFFFE`

These are installation-specific and are not assumed to have portable semantics.

They follow the same descriptor rule as experimental IDs.

## 13. Unknown capability handling

Forward compatibility is mandatory.

A bridge receiving a syntactically valid report for an unknown capability:

1. MUST NOT reject the whole NinaLink frame merely because the capability ID is
   unknown;
2. SHOULD forward the numeric capability ID and raw typed value;
3. SHOULD preserve descriptor metadata if it has previously been learned;
4. MAY expose the value as a generic Home Assistant entity if enough descriptor
   metadata is available;
5. MUST NOT silently reinterpret the value as another known capability.

This rule allows an old bridge to transport a capability introduced by a newer
node firmware.

## 14. Mapping of current B0 functionality

This mapping is descriptive only. B1.1 does not alter runtime code.

| B0 source/primitive | NinaLink semantic capability |
|---|---|
| Battery ADC / `BAT READ` | `battery_voltage` |
| DS18B20 / `TEMP READ` | `temperature` |
| LIS2DH12 motion | `motion` |
| LIS2DH12 TAP | `tap` |
| LIS2DH12 FALL | `fall` |
| LIS2DH12 XYZ | `acceleration_x/y/z` |
| Vibration metrics | `vibration_rms`, `vibration_peak`, `vibration_peak_to_peak`, `vibration_frequency` |
| VIB_AUTO alarm | `vibration_alarm` |
| Hall digital state | `hall_state` |
| Hall counter | `counter` |
| Hall quadrature | `quadrature_position` |
| Tracking runtime | `tracking_active` |

Legacy VM/NDP interfaces remain unchanged.

## 15. Home Assistant semantic intent

The final HA mapping is B7 work, but B1.1 defines the intended category:

| Capability kind | Typical HA entity |
|---|---|
| MEASUREMENT | `sensor` |
| STATE | `binary_sensor` or typed state entity |
| EVENT | HA event |
| COUNTER | `sensor` |
| POSITION | `sensor` |
| STATUS | `binary_sensor` / diagnostic entity |

The bridge must not embed HA-specific entity logic into the LoRa radio driver.

## 16. Out of scope for B1.1

B1.1 intentionally does **not** define:

- NinaLink frame bytes;
- message type numbers;
- CRC/MIC/authentication;
- node IDs;
- sequence numbers;
- ACK/retry behavior;
- LoRa RX windows;
- capability descriptor wire encoding;
- BLE/NDP bridge opcodes;
- VM generic capability opcodes;
- Home Assistant child-device implementation.

Those belong to B1.2 and later gates.

## 17. B1.1 acceptance criteria

B1.1 is PASS when:

- this document is reviewed and committed;
- no runtime source file is changed;
- B0 native/NDP/VM namespaces remain untouched;
- all currently relevant NINASENSE sensing functions have a semantic mapping;
- future external sensors can be represented without changing the fundamental
  registry structure;
- unknown/custom capabilities have an explicit forward-compatibility rule;
- `git diff b0-pre-ninalink-20260917..HEAD` contains only the intended B1.1
  documentation change.

Suggested commit:

```text
B1.1: define NinaLink capability registry v1
```
