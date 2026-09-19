# B5.3c — Persistent State Overwrite / Sequence Semantics

Persistent state is keyed by:

    (node_id, capability_id, channel)

CAP_REPORT updates persistent state. CAP_EVENT remains handled by the B5.3b
journal and never enters this overwrite path.

For an existing key, sequence numbers use 16-bit serial arithmetic:

    diff = (candidate - current) mod 65536
    newer iff 0 < diff < 32768

Thus increasing sequence overwrites, equal/stale/half-range does not, and
65535 -> 0 is accepted as forward progress.

Frame counters still record a valid stale CAP_REPORT that reached the consumer.
Only the persistent value, timestamp, sequence and update_count remain unchanged.
Freshness is per key, so an older report may still create a previously unseen
capability/channel.

Hardware gate:

    seq=1000 TEMPERATURE=1975
    seq=1002 TEMPERATURE=2025
    seq=1001 TEMPERATURE=1500   # deliberately late

All three frames must be ACKed and consumed. Final state must remain:

    Values=1
    Frame updates=3
    Reports=3
    Events=0
    Last sequence=1002
    TEMPERATURE[0] S16=2025 seq=1002 updates=2

Reboot/session limitation:
NinaLink v1 has no boot/session epoch. If a node independently reboots and
restarts its sequence stream while the bridge cache survives, serial ordering
cannot unambiguously distinguish reboot from an old frame. B5.3c intentionally
does not hide this with a time heuristic.
