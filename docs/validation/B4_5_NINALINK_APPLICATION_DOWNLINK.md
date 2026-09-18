# B4.5 — NinaLink Application Downlink

## Goal

Prove a real application command from bridge to battery node while preserving
the low-power "RX only after uplink" architecture.

First gate:

```text
CAP_SET tracking_active = OFF
```

OFF is selected first because it is safe, immediate and does not take over the
node's BLE advertising.

## Flow

```text
Host -> bridge: queue CAP_SET for node

Node                     Bridge
----                     ------
CAP_REPORT seq=N  -----> validate + queue
                         pending command matches node
                  <----- CAP_SET cmd=M reply_to=N
apply tracking OFF
                  -----> ACK cmd=M status=OK
sleep                    mark command DONE
                         resume continuous RX
```

The CAP_SET replaces the normal B4.4 ACK. Its `reply_to_seq` makes it an
implicit acknowledgment of the node uplink.

## Hardware gate

Start bridge:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-start
```

Queue OFF:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
  ninalink-cap-set-tracking --node 0xAD64D423 --value off
```

Trigger one reliable node contact:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-reliable-test \
  --window 600 --attempts 3 --backoff 200 --wait 6
```

Expected node transaction:

```text
Result: ACKED
Attempts: 1/3
RX/downlink frame: 22 bytes
```

Bridge:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-app-status
```

Expected:

```text
Pending: no
State: DONE
Result: OK
Sent total: 1
Completed total: 1
Result timeouts: 0
```

Node:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 ninalink-node-app-status
```

Expected:

```text
Command seen: yes
Result: OK
Applied total: 1
Duplicate total: 0
Tracking active: no
```

Bridge RX queue must still contain exactly one copy of the uplink CAP_REPORT.

A second reliable node contact with no pending application command must fall
back to the ordinary 18-byte ACK path, proving B4.3/B4.4 compatibility.

## B4.5 pass criteria

1. Existing codec/message host tests PASS.
2. ARM build PASS.
3. CAP_SET 0x30 is delivered only to the addressed node.
4. reply_to_seq matches and implicitly ACKs the triggering uplink.
5. tracking_active OFF is applied on the node.
6. Node returns an application-result ACK.
7. Bridge marks the command DONE/OK.
8. Bridge resumes continuous RX.
9. Node reports one application, zero duplicates.
10. Subsequent no-command uplink uses the normal 18-byte ACK.
11. B4.4 retry/dedup behavior remains operational.
