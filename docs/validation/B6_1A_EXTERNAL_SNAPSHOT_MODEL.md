# B6.1a — One-shot External Snapshot Model

## Goal

B6.1a is the first host-runtime layer above the frozen B5.4/B5.5 bridge API.
It does not modify firmware and it contains no Home Assistant-specific entity
logic.

Flow:

    NinaLink / LoRa
          |
          v
      B5.3 cache
          |
          v
      B5.4 selectors 30..35
          |
          v
    Application BLE / NDP
          |
          v
    B6.1a logical model

## Transport

B6 uses the Application BLE/NDP plane, not physical NUS. P0.21/NUS remains
maintenance/programming access.

## Model schema v1

Each capability uses a stable logical key:

    (node_id, capability_id, channel)

rendered as:

    0xNNNNNNNN:0xCCCC:channel

Known descriptors expose typed and engineering values using B5.4 scale/unit
metadata. Unknown capabilities remain numeric/raw.

## Event cursor

The initial model sets:

    event_cursor = event_window.newest_id

B6.1b will use that cursor when reconciling B5.5 EVENT notifications.

## CLI

    python3 nrfclaw_cli.py --device C8BA09       ninalink-external-model --json

No P0.21/NUS connection is required while the bridge is on Application/NDP.

## Acceptance

- deterministic/idempotent model from identical B5.4 data;
- stable node/capability ordering;
- unique `(node, capability, channel)` keys;
- correct descriptor scale/unit projection;
- event cursor initialized from B5.4 newest event;
- Application BLE/NDP transport;
- no changes to `src/`, `include/`, or `Makefile` relative to B5.5.
