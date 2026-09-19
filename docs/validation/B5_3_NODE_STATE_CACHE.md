# B5.3 — Node State Cache / Internal Queue Consumer

B5.3 removes the operational dependency on CLI draining of the B4.4 validated
queue while preserving the B4.4 reliability invariant:

    semantic validation
      -> validated queue admission
      -> remember/dedup
      -> internal consumer
      -> ACK/downlink

The internal consumer caches CAP_REPORT/CAP_EVENT by
`node_id + capability_id + channel` and immediately releases the admission
queue. Unknown/vendor capability IDs remain typed numeric values.

Capacity:
- 8 nodes
- 24 values per node

`ninalink-bridge-rx` now drains an independent best-effort diagnostic mirror
(depth 8); mirror overflow never blocks ACK/reliability.

CLI:
- `ninalink-consumer-status`
- `ninalink-state-cache --node 0xAD64D423`
- `ninalink-nodes-clear` clears registry + state cache.

Hardware gate: six reliable contacts without `ninalink-bridge-rx`. This exceeds
the old queue depth four. PASS requires all six ACKED, bridge Queued=0,
Bridge dropped=0, B5.2 READY, consumer Consumed=6/Cached frames=6/Cache errors=0,
and battery+temperature cached with six updates each.
