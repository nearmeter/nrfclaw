#!/usr/bin/env bash
set -euo pipefail
CLI="${CLI:-python3 nrfclaw_cli.py}"; BRIDGE="${BRIDGE:-C8BA09}"; NODE="${NODE:-1BF1D0}"; T="${TMPDIR_B54:-/tmp/nrfclaw-b54}"; mkdir -p "$T"
cleanup(){ $CLI --device "$BRIDGE" ninalink-auto-discovery on >/dev/null 2>&1 || true; }; trap cleanup EXIT
$CLI --device "$BRIDGE" ninalink-bridge-start
$CLI --device "$BRIDGE" ninalink-auto-discovery off
$CLI --device "$BRIDGE" ninalink-cache-clear
$CLI --device "$NODE" ninalink-reliable-test --window 600 --attempts 3 --backoff 200 --wait 6
$CLI --device "$NODE" ninalink-event-test --event tap --window 600 --attempts 3 --backoff 200 --wait 6
sleep 2
$CLI --device "$BRIDGE" ninalink-external-snapshot --json | tee "$T/snapshot.txt"
$CLI --device "$BRIDGE" ninalink-external-events --cursor 0 --limit 8 --json | tee "$T/events1.txt"
CURSOR="$(python3 - "$T/events1.txt" <<'PY2'
import json,pathlib,sys
s=pathlib.Path(sys.argv[1]).read_text();o=json.loads(s[s.find('{'):]);print(o['next_cursor'])
PY2
)"
echo "First cursor: $CURSOR"
$CLI --device "$NODE" ninalink-event-test --event fall --window 600 --attempts 3 --backoff 200 --wait 6
sleep 2
$CLI --device "$BRIDGE" ninalink-external-events --cursor "$CURSOR" --limit 8 --json | tee "$T/events2.txt"
python3 - "$T/snapshot.txt" "$T/events1.txt" "$T/events2.txt" <<'PY2'
import json,pathlib,sys
def L(p):
 s=pathlib.Path(p).read_text();return json.loads(s[s.find('{'):])
s=L(sys.argv[1]);e1=L(sys.argv[2]);e2=L(sys.argv[3]);assert s['schema']==1
n=next(x for x in s['nodes'] if x['node_id'].upper()=='0XAD64D423');ids={x['capability_id'].upper() for x in n['states']};assert {'0X0001','0X0100'}<=ids and '0X0201' not in ids
assert not e1['overrun'] and not e1['stream_reset'] and len(e1['events'])==1;t=e1['events'][0];assert t['capability_id'].upper()=='0X0201' and e1['next_cursor']==t['event_id']
assert not e2['overrun'] and not e2['stream_reset'] and len(e2['events'])==1;f=e2['events'][0];assert f['event_id']>t['event_id'] and f['capability_id']!=t['capability_id'] and e2['next_cursor']==f['event_id']
print('PASS external schema v1');print('PASS snapshot BATTERY_VOLTAGE + TEMPERATURE');print('PASS TAP event-only');print(f"PASS TAP cursor id={t['event_id']}");print(f"PASS incremental FALL id={f['event_id']}");print('PASS no repeated TAP after cursor')
PY2
$CLI --device "$BRIDGE" ninalink-consumer-status
$CLI --device "$BRIDGE" ninalink-external-snapshot
