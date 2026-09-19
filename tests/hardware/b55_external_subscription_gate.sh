#!/usr/bin/env bash
set -euo pipefail

CLI="${CLI:-python3 nrfclaw_cli.py}"
BRIDGE="${BRIDGE:-C8BA09}"
NODE="${NODE:-1BF1D0}"
STATE_DELAY="${STATE_DELAY:-25}"
EVENT_GAP="${EVENT_GAP:-8}"
SCAN_TIMEOUT="${SCAN_TIMEOUT:-5}"
WATCH_TIMEOUT="${WATCH_TIMEOUT:-50}"
T="${TMPDIR_B55:-/tmp/nrfclaw-b55-nodual}"

mkdir -p "$T"
WATCH="$T/watch.log"
NODESTAT="$T/node-status.log"
rm -f "$WATCH" "$NODESTAT"

echo "=== B5.5 no-dual-BLE gate ==="
echo "Bridge: $BRIDGE  Node: $NODE"
echo "STATE delay=${STATE_DELAY}s  EVENT gap=${EVENT_GAP}s"
echo
echo "IMPORTANT: P0.21/NUS must be available on bridge and node before starting."
echo

echo "=== 1. Prepare bridge over NUS ==="
$CLI --device "$BRIDGE" ninalink-bridge-stop || true
$CLI --device "$BRIDGE" ninalink-bridge-start
$CLI --device "$BRIDGE" ninalink-auto-discovery off
$CLI --device "$BRIDGE" ninalink-cache-clear

echo "=== 2. Quiesce + arm node over NUS ==="
$CLI --device "$NODE" ninalink-tx-stop || true
$CLI --device "$NODE" ninalink-b55-gate-arm \
    --state-delay "$STATE_DELAY" \
    --event-gap "$EVENT_GAP"

echo "=== 3. Release bridge NUS -> Application ==="
$CLI --device "$BRIDGE" ninalink-bridge-handoff
sleep 2

echo "=== 4. One persistent Application BLE connection; node BLE stays disconnected ==="
$CLI --device "$BRIDGE" \
    --scan-timeout "$SCAN_TIMEOUT" \
    ninalink-external-watch \
    --mask all \
    --count 2 \
    --timeout "$WATCH_TIMEOUT" \
    --json | tee "$WATCH"

echo "=== 5. Validate B5.5 notifications ==="
python3 - "$WATCH" <<'PY'
import json, pathlib, sys

rows=[]
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    line=line.strip()
    if line.startswith("{"):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass

sub=next((r for r in rows if r.get("type")=="subscription"), None)
changes=[r for r in rows if r.get("type")=="change"]
stats=next((r for r in rows if r.get("type")=="stats"), None)

if sub is None:
    raise SystemExit("FAIL missing subscription record")
if len(changes) != 2:
    raise SystemExit(f"FAIL expected 2 change notifications, got {len(changes)}: {changes}")
if stats is None:
    raise SystemExit("FAIL missing notification stats")

state=changes[0]
event=changes[1]

if not state.get("state_changed") or state.get("event_changed"):
    raise SystemExit(f"FAIL first notification is not STATE-only: {state}")
if not event.get("event_changed"):
    raise SystemExit(f"FAIL second notification does not include EVENT: {event}")
if state["state_revision"] <= sub["state_revision"]:
    raise SystemExit(f"FAIL state_revision did not advance: {sub} -> {state}")
if event["event_revision"] <= sub["event_revision"]:
    raise SystemExit(f"FAIL event_revision did not advance: {sub} -> {event}")
if event["newest_event_id"] < 1:
    raise SystemExit(f"FAIL newest_event_id did not advance: {event}")
if stats["notifications_sent"] < 2:
    raise SystemExit(f"FAIL notifications_sent < 2: {stats}")
if stats["send_errors"] != 0:
    raise SystemExit(f"FAIL send_errors != 0: {stats}")

print("PASS Application/NDP subscription established")
print("PASS deferred CAP_REPORT generated STATE notification")
print("PASS deferred CAP_EVENT generated EVENT notification")
print("PASS revisions advanced from subscription baseline")
print("PASS newest_event_id advanced")
print("PASS notifications_sent >= 2")
print("PASS send_errors == 0")
PY

echo "=== 6. Node evidence after Application watcher disconnects ==="
$CLI --device "$NODE" ninalink-b55-gate-status --json | tee "$NODESTAT"

python3 - "$NODESTAT" <<'PY'
import json, pathlib, sys

rows=[]
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    line=line.strip()
    if line.startswith("{"):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass

s=next((r for r in rows if r.get("type")=="b55_gate"), None)
if s is None:
    raise SystemExit("FAIL missing node deferred-gate status")
if s["stage"] != "DONE":
    raise SystemExit(f"FAIL node gate not DONE: {s}")
if s["error"] != "NONE":
    raise SystemExit(f"FAIL node gate error: {s}")
if s["state_result"] != 1 or not s["state_sent"]:
    raise SystemExit(f"FAIL node STATE was not ACKED: {s}")
if s["event_result"] != 1 or not s["event_sent"]:
    raise SystemExit(f"FAIL node EVENT was not ACKED: {s}")

print("PASS node STATE ACKED")
print("PASS node EVENT ACKED")
print("PASS node deferred gate DONE")
PY

echo
echo "B5.5 HARDWARE GATE (NO DUAL BLE): PASS"
