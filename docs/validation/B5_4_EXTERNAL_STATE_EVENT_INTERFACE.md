# B5.4 — External State/Event Interface

B5.4 is the stable read-only boundary above the validated B5.1/B5.2/B5.3 data model. It does not add Home Assistant-specific logic to the radio/bridge core.

Flow:

    NinaLink RF -> node registry/discovery -> B5.3 state + event journal
                                           -> B5.4 external facade
                                           -> NDP/CLI JSON
                                           -> future HA/API/automation

External schema version: 1.

Persistent state remains keyed by `(node_id, capability_id, channel)` and exposes raw typed value, sequence, age, update count and source message type. Standard capabilities obtain `kind`, `value_type`, `scale10`, `unit`, and behavior flags from the global semantic registry. Unknown/vendor/private IDs are never rejected or reinterpreted; they remain numeric/raw with `descriptor.known=false` until an authoritative bridge-side descriptor cache exists.

Events receive a monotonic bridge-runtime `event_id` independent of NinaLink sequence. `ninalink-cache-clear` empties the journal but does not rewind this cursor. The journal remains depth 8. Pollers persist `next_cursor`; CLI reports `overrun=true` if the cursor fell behind the oldest retained event and `stream_reset=true` if a bridge runtime reset makes the saved cursor newer than the current stream.

NDP_NINALINK_BRIDGE selectors (all payloads <=15 bytes):
- 30 EXTERNAL_INFO (12)
- 31 EXTERNAL_NODE(index) (15)
- 32 EXTERNAL_STATE(node,index) (15)
- 33 EXTERNAL_EVENT_NEXT_META(cursor) (15)
- 34 EXTERNAL_EVENT_VALUE(event_id) (7)
- 35 EXTERNAL_DESCRIPTOR(capability,channel) (6)

CLI:

    ninalink-external-snapshot
    ninalink-external-snapshot --json
    ninalink-external-events --cursor 0 --json

B5.4 is intentionally read-only. HA child-device/entity mapping, MQTT/REST, write operations, cloud transport and persistent custom descriptor storage remain later work.
