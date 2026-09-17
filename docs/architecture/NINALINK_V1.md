# NinaLink v1 Wire Format

Status: **B1.2 candidate**
Baseline: `b0-pre-ninalink-20260917` / `97c6749ffc50cac769aa17fb097bab9481cf54cf`
Capability registry: `docs/architecture/CAPABILITY_REGISTRY.md`, registry version 1.

## 1. Goals

NinaLink is the compact star-network protocol used between battery-powered
NINASENSE nodes and a continuously powered NINASENSE Bridge.

It is intentionally independent from:

- BLE/NDP;
- Home Assistant entity representation;
- VM bytecode;
- the physical sensor model (DS18B20, LIS2DH12, Hall, future sensors);
- the LoRa driver implementation.

B1.2 freezes the on-air ABI. B3 will implement encode/decode and B4 will bind it
to the LoRa bridge runtime.

## 2. Transport assumptions

The B0 LoRa backend accepts a maximum payload of 64 bytes per radio packet.

Therefore:

```text
NINALINK_MAX_FRAME = 64 bytes
```

NinaLink v1 does not fragment ordinary frames at the generic transport layer.
Messages that enumerate many capabilities use explicit pagination.

All multi-byte integer fields are **little-endian**.

## 3. Network topology

NinaLink v1 is a star protocol:

```text
Node 1 ─┐
Node 2 ─┤
Node 3 ─┼──── LoRa ──── NINASENSE Bridge
...     ─┤
Node N ─┘
```

`node_id` always identifies the sensor-node context:

- uplink: source node;
- downlink: destination node;
- bridge ACK to a node: the node being acknowledged.

The bridge itself does not require a node ID in v1.

Reserved node ID:

```text
0xFFFFFFFF = broadcast
```

## 4. Node identity

The default 32-bit NinaLink node ID is:

```text
NRF_FICR->DEVICEID[0]
```

The full Nordic 64-bit device identity is reported in `HELLO` as:

```text
device_id0 = NRF_FICR->DEVICEID[0]
device_id1 = NRF_FICR->DEVICEID[1]
```

This avoids creating a second persistent identity database merely for NinaLink.

A future provisioning layer may assign an alternate logical ID, but that is not
part of B1.2.

## 5. Network ID

NinaLink frames carry a 16-bit `network_id`.

Purpose:

- reject traffic from unrelated nearby NinaLink installations before higher
  layer processing;
- support multiple independent bridges on the same RF profile;
- provide a future provisioning boundary.

Reserved:

```text
0x0000 = unprovisioned / laboratory network
```

B3 may initially use `0x0000`. Provisioning/persistence of a production
`network_id` is B4+ work.

`network_id` is not a cryptographic credential.

## 6. Frame layout

Base header is 13 bytes.

```text
Offset  Size  Field
------  ----  --------------------------------
0       1     magic = 0x4E ('N')
1       1     protocol_version = 0x01
2       1     flags
3       1     message_type
4       2     network_id (LE)
6       1     payload_length
7       4     node_id (LE)
11      2     sequence (LE)
13      N     payload
13+N    2     CRC16-CCITT-FALSE (LE)
```

Total frame length:

```text
15 + payload_length
```

Maximum v1 payload:

```text
49 bytes
```

because:

```text
13-byte header + 49-byte payload + 2-byte CRC = 64 bytes
```

A receiver MUST reject a frame if:

- length < 15;
- magic != `0x4E`;
- protocol_version != `1`;
- payload length does not match the physical frame length;
- payload length > 49;
- CRC is invalid;
- flags contain a bit that v1 requires to be zero and the implementation does
  not support that extension.

## 7. CRC

Plain NinaLink v1 uses CRC-16/CCITT-FALSE:

```text
polynomial = 0x1021
init       = 0xFFFF
refin      = false
refout     = false
xorout     = 0x0000
```

The CRC covers every byte from `magic` through the final payload byte.

The CRC is transmitted little-endian.

The LoRa PHY CRC remains enabled; NinaLink CRC exists to validate the complete
protocol frame independently of the radio backend and for deterministic host
tests.

CRC does **not** provide authentication.

## 8. Flags

```text
0x01  ACK_REQ
0x02  MORE
0x04  AUTH       reserved in B1.2
0x08  ENCRYPTED  reserved in B1.2
0x10  reserved
0x20  reserved
0x40  reserved
0x80  reserved
```

### ACK_REQ

Sender requests an `ACK` or `NACK`.

Retransmission timing and RX-window policy are defined in B4, not B1.2.

### MORE

Additional pages/frames belonging to the same logical response follow.

### AUTH / ENCRYPTED

Reserved for a future NinaLink security profile.

B1.2/B3 implementations MUST transmit these bits as zero. A receiver that does
not implement the future security profile MUST NOT reinterpret frames carrying
these flags as ordinary plaintext traffic.

## 9. Sequence numbers

`sequence` is an unsigned 16-bit sequence number.

Rules:

- each node maintains an uplink sequence;
- the bridge maintains a separate downlink sequence per node;
- normal new transmissions increment the corresponding sequence modulo 65536;
- a retransmission of the same logical frame reuses the same sequence;
- duplicate suppression is based primarily on `(node_id, direction, sequence)`;
- after detecting a valid duplicate that requested ACK, the receiver should ACK
  it again but must not publish the same application event twice.

Persistence of sequence numbers across reset is not required by B1.2.
Security profiles may impose stronger rules later.

## 10. Message types

```text
0x01 HELLO
0x02 CAPS_REQUEST
0x03 CAPS_RESPONSE

0x10 CAP_REPORT
0x11 CAP_EVENT

0x20 ACK
0x21 NACK

0x30 CAP_READ      reserved for later implementation
0x31 CAP_SET       reserved for later implementation
0x32 COMMAND       reserved for later implementation

0x80-0xFF reserved
```

Unknown message types are not fatal to the radio session. If `ACK_REQ` is set,
a receiver may return `NACK/UNSUPPORTED`.

## 11. HELLO

Direction:

```text
node -> bridge
```

Payload, 19 bytes:

```text
Offset Size Field
0      4    device_id0
4      4    device_id1
8      1    capability_registry_version
9      1    advertised_capability_count
10     1    board_type
11     1    hw_rev_major
12     1    hw_rev_minor
13     1    fw_major
14     1    fw_minor
15     1    fw_patch
16     1    fw_prerelease
17     2    fw_build
```

The header `node_id` should equal `device_id0` for an unprovisioned/default
NINASENSE node.

`advertised_capability_count` counts concrete supported `(capability_id,
channel)` endpoints, not all IDs in the global registry.

## 12. CAPS_REQUEST

Direction:

```text
bridge -> node
```

Payload:

```text
Offset Size Field
0      1    page_index
```

Page zero is requested first.

The node replies with `CAPS_RESPONSE`.

## 13. CAPS_RESPONSE

Direction:

```text
node -> bridge
```

Payload header:

```text
Offset Size Field
0      1    registry_version
1      1    page_index
2      1    descriptor_count
3      1    reserved = 0
```

Each descriptor is 9 bytes:

```text
Offset Size Field
0      2    capability_id
2      1    channel
3      1    kind
4      1    value_type
5      1    scale10 (signed int8)
6      1    unit
7      1    behavior_flags
8      1    runtime_state_flags
```

A 49-byte payload holds:

```text
4-byte page header + 5 * 9-byte descriptors = 49 bytes
```

Therefore a CAPS_RESPONSE page contains at most **5 descriptors**.

`MORE` is set when a subsequent page exists.

## 14. Kind encoding

Frozen numeric encoding:

```text
0x01 MEASUREMENT
0x02 STATE
0x03 EVENT
0x04 COUNTER
0x05 POSITION
0x06 STATUS
```

`0x00` is invalid.

## 15. Value-type encoding

Frozen numeric encoding:

```text
0x01 BOOL
0x02 U8
0x03 S8
0x04 U16
0x05 S16
0x06 U32
0x07 S32
0x08 ENUM8
```

Value byte lengths:

```text
BOOL   1
U8     1
S8     1
ENUM8  1
U16    2
S16    2
U32    4
S32    4
```

Integer values are little-endian.

BOOL canonical encoding:

```text
0x00 = false
0x01 = true
```

Other BOOL values are invalid.

## 16. Unit encoding

Frozen v1 numeric encoding:

```text
0x00 NONE
0x01 BOOLEAN
0x02 PERCENT
0x03 VOLT
0x04 AMPERE
0x05 WATT
0x06 WATT_HOUR
0x07 CELSIUS
0x08 PASCAL
0x09 LUX
0x0A PPM
0x0B PPB
0x0C MILLI_G
0x0D HERTZ
0x0E SECOND
0x0F METER
0x10 METER_PER_SECOND
0x11 LITER
0x12 LITER_PER_MINUTE
0x13 RPM
0x14 COUNT
```

## 17. Runtime-state flags

Frozen v1 encoding:

```text
0x01 SUPPORTED
0x02 PRESENT
0x04 ENABLED
0x08 FAULT
```

Other bits are reserved.

## 18. Behavior flags

Frozen v1 encoding:

```text
0x01 READABLE
0x02 REPORTABLE
0x04 EVENT_SOURCE
0x08 WRITABLE
0x10 RETAINED
```

Other bits are reserved.

## 19. CAP_REPORT

Direction:

```text
node -> bridge
```

Used for measurements, states, counters, positions and statuses.

Payload:

```text
Offset Size Field
0      1    entry_count
1      ...  value entries
```

Each value entry:

```text
Offset Size Field
0      2    capability_id
2      1    channel
3      1    value_type
4      V    value
```

Entry length is `4 + value_size(value_type)`.

Including `value_type` in every report is intentional: an older bridge can
parse and forward a capability value even when it does not know the semantic ID.

The sender MUST use the value type frozen by the registry for standardized IDs.

## 20. CAP_EVENT

Direction:

```text
node -> bridge
```

Encoding is identical to CAP_REPORT.

CAP_EVENT exists separately so the bridge can preserve event semantics:

```text
tap
fall
other momentary occurrences
```

without inventing a persistent state at the NinaLink layer.

A bridge may later translate an event into a temporary HA binary sensor state,
but that policy is not part of NinaLink.

## 21. ACK

Direction:

```text
either direction
```

Payload:

```text
Offset Size Field
0      2    acknowledged_sequence
2      1    status
```

ACK status:

```text
0x00 OK
```

ACK does not carry application data in v1.

## 22. NACK

Payload:

```text
Offset Size Field
0      2    rejected_sequence
2      1    reason
```

Reason codes:

```text
0x01 BAD_FRAME
0x02 UNSUPPORTED
0x03 BUSY
0x04 UNAUTHORIZED
0x05 BAD_VALUE
0x06 INTERNAL
```

## 23. CAP_READ / CAP_SET / COMMAND

The message IDs are reserved by B1.2 to avoid a future ABI collision, but B3
does not need to implement them.

Planned minimal forms:

```text
CAP_READ:
  capability_id:u16
  channel:u8

CAP_SET:
  capability_id:u16
  channel:u8
  value_type:u8
  value:V
```

Authentication will be required before production use of writable/downlink
operations.

## 24. Unknown capabilities

For an unknown standardized/custom capability ID:

- frame validation still succeeds;
- value_type still permits value parsing;
- the bridge forwards `(node_id, capability_id, channel, value_type, raw_value)`;
- if a descriptor was learned from CAPS_RESPONSE, scale/unit/kind are retained;
- the bridge must not guess a known semantic identity.

This is a hard forward-compatibility requirement.

## 25. RF metadata

RSSI and SNR are not node capabilities.

The bridge attaches them as receive metadata:

```text
rssi_dbm_x2
snr_db_x4
```

This matches the existing B0 LoRa receive API and avoids pretending that a node
measured its own uplink RSSI/SNR.

## 26. B1.2 acceptance criteria

B1.2 PASS requires:

- the frame layout and numeric encodings above are reviewed and committed;
- maximum frame size remains <= 64 bytes;
- capability IDs remain exactly those frozen in B1.1;
- B0 NDP/VM/native capability ABI is untouched;
- no runtime source code is changed by the B1.2 commit;
- a future B3 encoder/decoder can be implemented without ambiguity.

Suggested commit:

```text
B1.2: define NinaLink v1 wire format
```
