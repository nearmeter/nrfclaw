#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

s = Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()

checks = [
    (
        "Application discovery rejects explicit NUS advertised name",
        'if name.upper().startswith("NRFCLAW(NUS)-"):' in s,
    ),
    (
        "NUS discovery rejects explicit NDP advertised name",
        'if upper_name.startswith("NRFCLAW(NDP)-"):' in s,
    ),
    (
        "NUS UUID fallback rejects simultaneous APP UUID",
        'if not address_match or not nus_match or app_match:' in s,
    ),
    (
        "exact NUS name remains authoritative",
        'if name_match:\n            score = 100' in s,
    ),
]

failed = 0
for name, cond in checks:
    if cond:
        print(f"PASS {name}")
    else:
        print(f"FAIL {name}")
        failed += 1

if failed:
    raise SystemExit(1)

print("BLE plane discovery gate: PASS")
PY
