# B5.2 — Automatic Node Discovery

B5.2 reuses the validated B4.9/B4.10 discovery engines. No NinaLink RF ABI
changes are introduced.

State machine:

    NEW -> CAPS_PENDING -> CAPS_DONE -> COMMANDS_PENDING -> READY

Discovery is contact-driven. A request is armed only when that node makes a
normal uplink, and it replaces the normal ACK inside the existing receive
window. Battery nodes are never kept awake by the bridge.

Per-node RAM metadata stores capability/command counts, registry versions,
next page/index, retry count and last discovery error.

Automatic discovery defaults ON when the bridge starts. Use:

    ninalink-auto-discovery status|on|off

Manual B4.9/B4.10 behavior remains available when automatic discovery is OFF.

Automatic timeouts are intentionally released after the B4 result window so a
sleeping node cannot monopolize the single bridge discovery engine. The same
page is retried with a fresh discovery request sequence on a later contact.
Manual B4.9/B4.10 same-sequence replay/dedup semantics are unchanged.

For 1BF1D0 the expected clean progression is:

    contact 1 -> caps=5,  next page=1
    contact 2 -> caps=10, next page=2
    contact 3 -> caps=15, next page=3
    contact 4 -> caps=17, CAPS_DONE
    contact 5 -> READY, commands=3

## B4.4 validated-queue interaction

B5.2 does not weaken the frozen B4.4 admission invariant:

A unique valid uplink is dedup-remembered and answered only after it
successfully enters the bridge validated queue.

Therefore, until a later state-cache/consumer stage drains that queue
automatically, a hardware discovery gate that generates more than
`NRFCLAW_NINALINK_BRIDGE_QUEUE_DEPTH` unique reports must drain the diagnostic
queue between groups of contacts.

A queue-full uplink must not advance B5.2 discovery state. The B5.2 admission
hotfix enforces this by invoking automatic discovery only after successful
queue admission (or for an already remembered duplicate).
