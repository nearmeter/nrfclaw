#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

python3 -m py_compile \
  integrations/home-assistant/custom_components/nrfclaw/ninalink_semantics.py \
  tests/host/test_b71_ha_semantics.py \
  NRFCLAW_CLI/nrfclaw_cli.py

python3 tests/host/test_b71_ha_semantics.py

CHANGED="$(git diff --name-only b5.5-external-change-20260919 -- src include Makefile)"
if [[ -n "$CHANGED" ]]; then
  echo "FAIL B7.1 touched frozen firmware/build files:"
  echo "$CHANGED"
  exit 1
fi

grep -q 'regular_devices_via_bridge' \
  integrations/home-assistant/custom_components/nrfclaw/ninalink_semantics.py
grep -q 'async def run_ninalink_ha_preview(args):' NRFCLAW_CLI/nrfclaw_cli.py

echo "PASS B7.1 remains host/Home-Assistant-side only"
echo "PASS HA preview Application route present"
echo "B7.1 source gate: PASS"
