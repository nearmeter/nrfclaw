# B4.4 — NinaLink Reliability Layer

## Goal

Add bounded retransmission and duplicate suppression on top of B4.3.

A new transaction allocates one sequence number. Every retransmission reuses
the same sequence and the exact same encoded wire image.

Default lab profile:

```text
ACK window:      600 ms
max attempts:    3
base backoff:    200 ms
```

Backoff is exponential and capped at 4000 ms:

```text
timeout after attempt 1 -> 200 ms
timeout after attempt 2 -> 400 ms
timeout after attempt 3 -> exhausted
```

The radio is not held in RX during backoff.

## Bridge duplicate suppression

The bridge keeps eight recent accepted keys:

```text
(network_id, node_id, message_type, sequence)
```

A duplicate is validated but is not added to the semantic queue again. If
`ACK_REQ` is set, the bridge ACKs the duplicate again.

A unique reliable frame is ACKed only after it successfully enters the
validated queue. Queue-full frames are not remembered and not ACKed, allowing
the sender to retry later instead of silently losing acknowledged data.

## Deterministic dropped-ACK gate

`ninalink-bridge-drop-next-ack` suppresses exactly the first ACK for the next
unique ACK-requesting frame. The frame is still accepted, queued once and
remembered.

The node must then timeout, back off and transmit the exact same sequence. The
bridge must classify that retransmission as duplicate, not queue it again, and
send the ACK.

Expected default gate:

```text
node:
  result       ACKED
  attempts     2/3
  retry +1
  timeout +1

bridge:
  received          2
  valid unique      1
  duplicates        1
  queued            1
  ACK sent          1
  test ACK drops    1
```

Draining the bridge must produce exactly one semantic CAP_REPORT.

## Negative exhaustion gate

With the bridge stopped:

```bash
ninalink-reliable-test --window 600 --attempts 3 --backoff 200 \
    --wait 6 --expect-timeout
```

Expected:

```text
Result: TIMEOUT
Attempts: 3/3
Retry total: +2
Timeout total: +3
Expected timeout observed: PASS
```

Application downlinks (`CAP_SET`, `COMMAND`) remain outside B4.4.
