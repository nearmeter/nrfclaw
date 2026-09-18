# B4.10 — Capability Discovery + Writable Metadata

Host gates:
- run_ninalink_capability_discovery.sh
- test_ninalink_capability_discovery_wire.py
- all B4.9/B3 codec/message suites.

Hardware:
1. Discover page 0.
2. Paginate while MORE is set, draining the four-entry bridge uplink queue
   regularly.
3. Find TRACKING_ACTIVE 0x0401 and verify BOOL/scale0/BOOLEAN plus WRITABLE.
4. Re-run CAP_SET tracking OFF and verify Result OK.
5. Suppress one discovery response, verify same request_seq retry,
   Served unchanged on duplicate, Duplicate +1, bridge Sent=2/Completed=1/
   timeout=1.
6. Drain queue and verify normal 18-byte ACK compatibility.
