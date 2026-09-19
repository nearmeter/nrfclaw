#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

python3 -m py_compile \
  NRFCLAW_CLI/nrfclaw_external.py \
  NRFCLAW_CLI/nrfclaw_cli.py \
  tests/host/test_b61b_live_reconcile.py

python3 tests/host/test_b61b_live_reconcile.py

CHANGED="$(git diff --name-only b5.5-external-change-20260919 -- src include Makefile)"
if [[ -n "$CHANGED" ]]; then
  echo "FAIL B6.1b modified frozen firmware/build files:"
  echo "$CHANGED"
  exit 1
fi

echo "PASS frozen B5.5 firmware/build tree untouched"

grep -q 'class NinaLinkExternalReconciler:' NRFCLAW_CLI/nrfclaw_external.py
grep -q 'async def run_ninalink_external_reconcile(args):' NRFCLAW_CLI/nrfclaw_cli.py
grep -q '"ninalink-external-reconcile"' NRFCLAW_CLI/nrfclaw_cli.py

echo "PASS Application-plane live reconcile route present"
echo "B6.1b source gate: PASS"
