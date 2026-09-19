# B5.1 — Bridge Node Registry

B5.1 adds a RAM-only node registry above the validated NinaLink bridge RX.

A node is observed after core decode + semantic validation and before duplicate
suppression. Thus a valid retransmission refreshes last_seen/RSSI/SNR without
changing B4.x dedup/reliability semantics.

Capacity: 8 nodes. Full registry uses least-recently-observed eviction.

Stored:
- node ID
- network ID
- last NinaLink sequence
- last NinaLink message type
- valid RF frames seen
- last seen
- RSSI x2
- SNR x4

Node ID 0 and broadcast 0xFFFFFFFF are ignored.

Bridge stop/start preserves the RAM registry. Reset/power-cycle clears it.
CLI can explicitly clear it.

NDP 0x5A actions:
- 17 registry status
- 18,index node core
- 19,index node link
- 20 clear

CLI:
- ninalink-nodes
- ninalink-nodes-clear

B5.1 intentionally does not yet persist nodes, auto-discover capabilities or
commands, cache values, or expose Home Assistant child entities.
