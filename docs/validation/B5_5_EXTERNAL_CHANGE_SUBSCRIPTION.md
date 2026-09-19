# B5.5 — External Change Notification / Subscription

## Goal

B5.4 remains the source of truth for nodes, state and events. B5.5 adds only a
lightweight connection-scoped notification that tells an external consumer
that B5.4 should be reconciled.

## Change domains

- STATE = 0x01
- EVENT = 0x02
- ALL   = 0x03

The B5.3 cache maintains three bridge-runtime uint32 revisions:

- state_revision
- event_revision
- change_revision

A CAP_REPORT increments state_revision once only when the externally visible
persistent snapshot changes. A stale/equal B5.3c report does not increment it.
A CAP_EVENT increments event_revision once per admitted event frame. Cache clear
bumps the affected domains but does not reset the monotonic revision counters.

## Async frame

Application BLE TX carries an unsolicited fixed 20-byte frame, deliberately
not shaped as NDP response:

    byte 0      magic 0xE5
    byte 1      schema 1
    byte 2      changed flags
    byte 3      subscription mask
    bytes 4..7  state_revision u32 LE
    bytes 8..11 event_revision u32 LE
    bytes12..15 newest_event_id u32 LE
    bytes16..19 change_revision u32 LE

The frame contains no sensor payload. The consumer reads B5.4 snapshot/events
after notification. Multiple mutations may coalesce into one notification;
revisions always describe the latest state.

## Subscription lifetime

Subscription is connection-scoped. Disconnect clears it. On reconnect:

1. subscribe;
2. read/reconcile B5.4 snapshot and event cursor;
3. process B5.5 notifications;
4. on every notification, reconcile B5.4 again.

This makes notification loss/coalescing harmless: B5.4 is authoritative.

## Application security boundary

Historically NDP_NINALINK_BRIDGE was physical-NUS-only. B5.5 exposes only
selectors 30..38 on the Application plane:

- 30..35 B5.4 read-only external API
- 36 B5.5 subscription status
- 37 B5.5 set subscription mask
- 38 B5.5 diagnostics

All older bridge control/test/clear selectors remain NUS-only. If an NDP owner
key is provisioned, the existing Application CONTROL authentication is still
required before these selectors are reached.

Selector 39 is NUS-only and performs a BLE ownership handoff after bridge setup:
it marks the programming session released while keeping the current response
alive. When the CLI disconnects, the existing BLE disconnect path resumes the
Application/NDP advertiser; bridge RX remains active.

## CLI

Normal Application-plane watch:

    python3 nrfclaw_cli.py --device C8BA09 \
      ninalink-external-watch --mask all --count 2 --json

Physical-NUS bridge handoff:

    python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-handoff

## Hardware gate

From NRFCLAW_CLI, after P0.21 is available on bridge and node:

    ../tests/hardware/b55_external_subscription_gate.sh

The gate starts bridge RX over NUS, releases BLE ownership to Application,
subscribes on Application BLE, then sends one CAP_REPORT and one TAP CAP_EVENT
from the node. It requires one STATE and one EVENT notification.

## No-dual-BLE hardware gate

The final B5.5 hardware gate does not require two simultaneous GATT links.

A NUS-only lab command on the node arms two deferred actions in RAM:

1. after `state_delay`, reuse the production reliable CAP_REPORT path;
2. after that report is ACKed, wait `event_gap` and reuse the validated
   synthetic TAP CAP_EVENT path.

The node BLE link is closed before the bridge is handed from NUS to
Application/NDP. During notification validation there is only one BLE
connection:

    node --NinaLink/LoRa--> bridge --Application BLE/NDP--> host

The deferred gate contains no direct LoRa/LLCC68 operations and does not build
its own NinaLink frames. It only schedules the existing link-layer APIs.
