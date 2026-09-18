# B4.6 — Reliable Application Downlink

## Goal

Prove end-to-end reliability when a CAP_SET is applied by the node but the
node's application-result ACK is lost.

```text
Node                                  Bridge
----                                  ------
CAP_REPORT seq=U1  -----------------> unique uplink
                         <----------- CAP_SET cmd=C1 reply_to=U1
apply CAP_SET once
cache (C1,result=OK)
[result ACK deliberately suppressed]
                                      result RX timeout
                                      keep CAP_SET C1 pending

CAP_REPORT seq=U2  -----------------> later uplink
                         <----------- SAME CAP_SET cmd=C1 reply_to=U2
detect duplicate C1
DO NOT apply again
duplicate_count++
send cached result OK  -------------->
                                      command DONE
```

## Reliability invariants

- Result timeout does not allocate a new command sequence.
- Resend reuses the same `command_seq` and requested value.
- `reply_to_seq` changes to match the new uplink opening the receive window.
- Node duplicate detection is by application `command_seq`.
- Duplicate CAP_SET is not applied twice.
- Node replays the cached application result.
- Bridge marks DONE only after receiving that result.

## Deterministic gate

Arm the node result loss:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-node-drop-next-app-result
```

Queue the command:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
  ninalink-cap-set-tracking --node 0xAD64D423 --value off
```

First contact:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-reliable-test --window 600 --attempts 3 --backoff 200 --wait 6
```

The node must finish ACKED because the CAP_SET itself implicitly ACKed the
uplink. After the bridge's result window expires:

```text
bridge: Pending=yes, Sent=1, Completed=0, Result timeouts=1
node:   Applied=1, Duplicate=0, Result drops=1
```

Second contact repeats the same command sequence:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-reliable-test --window 600 --attempts 3 --backoff 200 --wait 6
```

Expected:

```text
bridge: Pending=no, DONE/OK, same command seq, Sent=2, Completed=1
node:   Applied=1, Duplicate=1
```

`Applied == 1` is the critical proof.

A third no-command contact must return to the ordinary 18-byte ACK path.
