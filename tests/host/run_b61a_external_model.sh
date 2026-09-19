#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

python3 -m py_compile   NRFCLAW_CLI/nrfclaw_external.py   NRFCLAW_CLI/nrfclaw_cli.py   tests/host/test_b61a_external_model.py

python3 tests/host/test_b61a_external_model.py

if git rev-parse -q --verify b5.5-external-change-20260919 >/dev/null; then
  CHANGED="$(git diff --name-only b5.5-external-change-20260919 -- src include Makefile)"
  if [[ -n "$CHANGED" ]]; then
    echo "FAIL B6.1a modified firmware/build files relative to B5.5:"
    echo "$CHANGED"
    exit 1
  fi
  echo "PASS frozen B5.5 firmware/build tree untouched"
else
  echo "FAIL missing tag b5.5-external-change-20260919"
  exit 1
fi

grep -q 'async def run_ninalink_external_model(args):' NRFCLAW_CLI/nrfclaw_cli.py
grep -q '"ninalink-external-model"' NRFCLAW_CLI/nrfclaw_cli.py

echo "PASS Application-plane CLI route present"
echo "B6.1a source gate: PASS"
