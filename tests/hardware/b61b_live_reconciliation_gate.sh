#!/usr/bin/env bash
set -euo pipefail

CLI="${CLI:-python3 nrfclaw_cli.py}"
BRIDGE="${BRIDGE:-C8BA09}"
NODE="${NODE:-1BF1D0}"
STATE_DELAY="${STATE_DELAY:-60}"
EVENT_GAP="${EVENT_GAP:-8}"
T="${TMPDIR_B61B:-/tmp/nrfclaw-b61b}"
mkdir -p "$T"
LOG="$T/reconcile.log"
rm -f "$LOG"

echo "=== B6.1b live reconciliation no-dual-BLE gate ==="

echo "=== 1. Prepare bridge over NUS ==="
$CLI --device "$BRIDGE" ninalink-bridge-stop || true
$CLI --device "$BRIDGE" ninalink-bridge-start
$CLI --device "$BRIDGE" ninalink-auto-discovery off
$CLI --device "$BRIDGE" ninalink-cache-clear

echo "=== 2. Arm autonomous node STATE + EVENT ==="
$CLI --device "$NODE" ninalink-tx-stop || true
$CLI --device "$NODE" ninalink-b55-gate-arm \
  --state-delay "$STATE_DELAY" \
  --event-gap "$EVENT_GAP"

echo "=== 3. Handoff bridge to Application/NDP ==="
$CLI --device "$BRIDGE" ninalink-bridge-handoff
sleep 2

echo "=== 4. Single persistent Application connection ==="
$CLI --device "$BRIDGE" \
  ninalink-external-reconcile \
  --count 2 \
  --timeout 50 \
  --json | tee "$LOG"

echo "=== 5. Validate reconciled model ==="
python3 - "$LOG" <<'PY'
import json
import pathlib
import sys

rows=[]
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    line=line.strip()
    if line.startswith("{"):
        rows.append(json.loads(line))

initial=next(r for r in rows if r.get("type")=="initial")
rec=[r for r in rows if r.get("type")=="reconcile"]
assert len(rec)==2, rec

state=next(r for r in rec if r.get("state_changed"))
event=next(r for r in rec if r.get("event_changed"))

assert state["state_refreshed"] is True, state
assert state["event_cursor"] == initial["event_cursor"], (initial,state)
assert state["state_revision"] > initial["state_revision"], (initial,state)

nodes=state["model"]["nodes"]
node=next(n for n in nodes if n["node_id"].upper()=="0XAD64D423")
temp=next(c for c in node["capabilities"] if c["capability_id"].upper()=="0X0100")
assert abs(float(temp["value"])-21.81) < 1e-9, temp

assert event["event_revision"] > initial["event_revision"], (initial,event)
assert event["event_cursor"] > initial["event_cursor"], (initial,event)
assert not event["overrun"], event
assert not event["stream_reset"], event
assert len(event["events"]) >= 1, event
tap=next(e for e in event["events"] if e["capability_id"].upper()=="0X0201")
assert tap["name"]=="TAP", tap

print("PASS initial model established after subscription")
print("PASS STATE notification reconciled authoritative temperature=21.81 C")
print("PASS STATE reconcile preserved event cursor")
print("PASS EVENT notification drained TAP from B5.4 journal")
print("PASS event cursor advanced")
print("PASS no overrun / stream reset")
print("B6.1b HARDWARE GATE: PASS")
PY
