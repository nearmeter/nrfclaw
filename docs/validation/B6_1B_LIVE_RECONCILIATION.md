# B6.1b — Live Reconciliation

B6.1b turns the one-shot B6.1a model into a live external consumer.

The transport remains Application BLE/NDP and firmware remains frozen at B5.5.

## Algorithm

    subscribe B5.5 ALL
          |
          v
    read B5.4 initial snapshot
          |
          +---- STATE hint ----> rebuild authoritative B5.4 state model
          |
          +---- EVENT hint ----> drain B5.4 journal after event_cursor

The B5.5 20-byte notification is never treated as sensor data. It is only a
reconciliation hint.

STATE and EVENT reconciliation are independent. In particular, rebuilding a
state snapshot must not advance the event cursor, because an event can occur
concurrently with the state refresh.

## Cursor rules

- initial cursor = B5.4 newest_event_id at initial snapshot;
- EVENT reconciliation drains strictly after the saved cursor;
- successful drain advances cursor to last returned event_id;
- repeated reconciliation is idempotent and does not replay an event;
- overrun and stream-reset remain explicit output states.

## CLI

    python3 nrfclaw_cli.py --device C8BA09 \
      ninalink-external-reconcile --count 2 --timeout 50 --json

## Final hardware topology

    node --NinaLink/LoRa--> bridge --Application BLE/NDP--> host

The no-dual-BLE deferred STATE+EVENT generator from B5.5 is reused as the
stimulus, but B6.1b validates the host reconciliation model rather than only the
notification frames.
