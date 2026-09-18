# B4.8 — Command Registry + Real Commands

Host gates:

```bash
./tests/host/run_ninalink_command_registry.sh
python3 tests/host/test_ninalink_command_wire.py
./tests/host/run_ninalink_codec.sh
./tests/host/run_ninalink_messages.sh
```

Hardware A — GET_NODE_INFO:

```bash
python3 nrfclaw_cli.py --device C8BA09   ninalink-command-node-info --node 0xAD64D423
```

After one reliable node contact, expect a 20-byte COMMAND downlink and an
8-byte result decoded as version=1, max frame=64, command count=3,
feature_flags=0x0F and node_id=0xAD64D423.

Hardware B — GET_TRACKING_STATE:

```bash
python3 nrfclaw_cli.py --device C8BA09   ninalink-command-tracking-state --node 0xAD64D423
```

With tracking OFF, expect Result=OK, result_len=1 and `Tracking active: no`.

Hardware C — registry errors:

```bash
python3 nrfclaw_cli.py --device C8BA09   ninalink-command --node 0xAD64D423 --id 0x7FFF
```

Expect `UNSUPPORTED`, result_len=0.

```bash
python3 nrfclaw_cli.py --device C8BA09   ninalink-command --node 0xAD64D423 --id 0x0003 --data 00
```

Expect `BAD_ARGS`, result_len=0. Neither case increments Executed.

Hardware D — replay through registry:

Arm next result loss, queue GET_NODE_INFO, perform two node contacts. The
second delivery must keep the same command_seq, increment Duplicate once,
not increment Executed a second time, and return the cached 8-byte result.

Compatibility: with no downlink pending, a final reliable contact must use
the ordinary 18-byte ACK.

## B4.8.1 Deferred bridge RX rearm

Hardware testing exposed a race after a COMMAND_RESULT/replay transaction:
the immediate restart of continuous LLCC68 RX could occur before timed-RX
cleanup made the radio idle. The previous behavior treated that transient
failure as fatal and disabled the bridge. A subsequent command then remained
`PENDING` with `Sent=0` while node uplinks timed out.

The bridge now defers/retries continuous-RX rearm until the radio is idle.
