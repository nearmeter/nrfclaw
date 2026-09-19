# B5.3d — Node Session Epoch / Reboot Semantics

## Problem

B5.3c correctly orders a 16-bit sequence stream within one node runtime, but
NinaLink v1 explicitly does not require sequence persistence across reset.
Therefore a node reboot can restart at sequence 1 while a live bridge still
holds a much larger last sequence and old B4.4 duplicate keys.

## Compatible extension

B5.3d does NOT change the frozen NinaLink v1 header and does NOT extend the
19-byte HELLO payload.  Instead, every link-layer CAP_REPORT and CAP_EVENT
carries one additional typed value entry:

    capability_id = 0xC001   (local/private range)
    channel       = 0
    type          = U32
    value         = session_id

This entry is transport metadata. It is stripped by the bridge consumer and is
never published as a semantic capability, persistent value or event.

Normal frame-size impact:

    CAP_REPORT battery+temperature: 28 -> 36 bytes
    CAP_EVENT TAP/FALL:             21 -> 29 bytes
    B5.3c state-test:               22 -> 30 bytes

The B4.2 explicit 64-byte max-frame lab generator remains a legacy lab frame and
is intentionally not expanded.

## Node session ID

The node obtains a fresh 32-bit session ID from the SoftDevice application RNG
pool.  It is RAM-only and therefore changes after a real reboot/power cycle.
No flash write is added to the low-power path.

Lab commands can force a deterministic session without modifying the normal
m_next_sequence stream:

    ninalink-session-status
    ninalink-session-force --id 0x11111111

## Bridge ordering

For a sessionized CAP_REPORT/CAP_EVENT:

    decode + semantic validation
      -> extract session metadata
      -> if same session: normal B4.4 duplicate check
      -> if new session: bypass old-session duplicate keys
      -> validated queue admission
      -> ONLY AFTER admission:
           commit session
           clear old duplicate keys for that node
      -> remember frame
      -> B5.3 consumer
      -> ACK/downlink

This preserves the B4.4 invariant: queue-full frames are neither remembered nor
used to commit a session transition and are not ACKed.

## State cache epoch

A session change invalidates the previous persistent values for that node and
resets its sequence-ordering epoch. Frame/report/event counters remain
cumulative for diagnostics.

Within the new session, the B5.3c serial-number rule remains unchanged:

    newer iff 0 < (candidate - current mod 65536) < 32768

## Deterministic hardware gate

    session A = 0x11111111
      seq1 TEMP=1975
      seq3 TEMP=2025

    session B = 0x22222222
      seq1 TEMP=2100  # intentionally collides with old session-A seq1
      seq0 TEMP=1500  # stale inside session B

All four frames must be ACKed/consumed. Session-B seq1 must bypass the old B4.4
duplicate key, start a fresh state epoch and become the current value. Session-B
seq0 must then be ignored by B5.3c ordering.

Expected final cache:

    Values=1
    Frame updates=4
    Reports=4
    Events=0
    Session=0x22222222
    Session changes=1
    Last sequence=1
    TEMPERATURE[0] S16=2100 seq=1 updates=1

A secondary real-reboot gate may then reset only the node while leaving the
bridge alive and verify that the RNG session changes and the first low-sequence
report is accepted without clearing the bridge.
