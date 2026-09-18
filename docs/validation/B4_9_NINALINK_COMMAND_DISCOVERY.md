# B4.9 — Command Discovery / Registry Advertisement

## Host gates

```bash
./tests/host/run_ninalink_command_discovery.sh
python3 tests/host/test_ninalink_command_discovery_wire.py
./tests/host/run_ninalink_command_registry.sh
python3 tests/host/test_ninalink_command_wire.py
./tests/host/run_ninalink_codec.sh
./tests/host/run_ninalink_messages.sh
```

Expected B4.9 registry discovery tests: all PASS.

## Hardware gate A — page zero

Start a clean bridge and queue discovery:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-start

python3 nrfclaw_cli.py --device C8BA09 \
  ninalink-command-discover \
  --node 0xAD64D423 \
  --start 0
```

Trigger one reliable contact:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-reliable-test \
  --window 600 --attempts 3 --backoff 200 --wait 6
```

Expected node downlink frame: 18 bytes (`COMMANDS_REQUEST`).

Then:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
  ninalink-command-discovery-status
```

Expected:

```text
Pending: no
State: DONE
Registry version: 1
Start index: 0
Total commands: 3
Returned: 3
Sent total: 1
Completed total: 1
Result timeouts: 0

0x0001 ECHO_U32           args 4..4 result<=4 READ_ONLY
0x0002 GET_NODE_INFO      args 0..0 result<=8 READ_ONLY
0x0003 GET_TRACKING_STATE args 0..0 result<=1 READ_ONLY
```

Node status:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-node-command-discovery-status
```

Expected unique served count +1, duplicate unchanged.

## Hardware gate B — pagination

Queue start index 2 and perform one node contact:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
  ninalink-command-discover \
  --node 0xAD64D423 \
  --start 2
```

Expected response metadata:

```text
Start index: 2
Total commands: 3
Returned: 1
```

and only `GET_TRACKING_STATE`.

## Hardware gate C — reliable discovery response

Arm result loss:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  ninalink-node-drop-next-command-result
```

Queue discovery page zero, then perform one node contact. The node receives the
18-byte request, but its COMMANDS_RESPONSE is suppressed.

After the bridge result window expires:

```text
Pending: yes
Sent total: 1
Completed total: 0
Result timeouts: 1
```

Perform a second node contact.

Expected:

```text
same discovery request sequence
bridge Sent total: 2
bridge Completed total: 1
bridge Result timeouts: 1
node Served count increases only once
node Duplicate count increases once
```

The returned descriptor page must match the first page.

## Compatibility

With no application/discovery downlink pending, a final reliable contact must
again receive the ordinary 18-byte ACK.
