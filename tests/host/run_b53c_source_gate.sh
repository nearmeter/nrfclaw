#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY2'
from pathlib import Path

cache = Path("src/nrfclaw_ninalink_state_cache.c").read_text()
link = Path("src/nrfclaw_ninalink_link.c").read_text()
ndp = Path("src/nrfclaw_ndp.c").read_text()
cli = Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()

checks = [
    ("serial comparator", "diff != 0U && diff < 0x8000U" in cache),
    ("per-key stale guard", "!sequence_newer(" in cache and "continue;" in cache),
    ("node summary non-regression", "sequence_newer(sequence, node->pub.last_sequence)" in cache),
    ("forced state builder", "build_state_test" in link and "NRFCLAW_SEMCAP_TEMPERATURE" in link),
    ("forced state starter", "nrfclaw_ninalink_link_start_state_test" in link),
    ("LINK selector 13", "if(p[0]==13U)" in ndp and "nrfclaw_ninalink_link_start_state_test" in ndp),
    ("CLI state test", "async def ninalink_state_test(" in cli and "bytes([13])" in cli),
]

for name, ok in checks:
    if not ok:
        raise SystemExit(f"FAIL {name}")
    print(f"PASS {name}")
PY2
