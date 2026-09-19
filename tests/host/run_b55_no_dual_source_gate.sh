#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
from pathlib import Path

src = Path("src/nrfclaw_b55_gate.c").read_text()
hdr = Path("include/nrfclaw_b55_gate.h").read_text()
main = Path("src/main.c").read_text()
ndp = Path("src/nrfclaw_ndp.c").read_text()
cli = Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()
mk = Path("Makefile").read_text()

checks = [
    ("deferred module built", "src/nrfclaw_b55_gate.c" in mk),
    ("main-loop processing", "nrfclaw_b55_gate_process();" in main),
    ("reuses deterministic CAP_REPORT path", "nrfclaw_ninalink_link_start_state_test(" in src),
    ("STATE gate independent of sensor priming", "nrfclaw_ninalink_link_start_reliable(" not in src),
    ("reuses CAP_EVENT test path", "nrfclaw_ninalink_link_start_event_test(" in src),
    ("no direct LoRa coupling", "nrfclaw_lora_" not in src and "llcc68" not in src.lower()),
    ("single-shot app timer", "APP_TIMER_MODE_SINGLE_SHOT" in src),
    ("selector 15 arm", "if(p[0]==15U)" in ndp and "nrfclaw_b55_gate_arm" in ndp),
    ("selector 16 status", "if(p[0]==16U)" in ndp and "nrfclaw_b55_gate_get_status" in ndp),
    ("CLI arm", "ninalink-b55-gate-arm" in cli),
    ("CLI status", "ninalink-b55-gate-status" in cli),
    ("finite state machine", "NRFCLAW_B55_GATE_DONE" in hdr and "NRFCLAW_B55_GATE_ERROR" in hdr),
]
for name, ok in checks:
    if not ok:
        raise SystemExit(f"FAIL {name}")
    print(f"PASS {name}")

print("B5.5 no-dual-BLE source gate: PASS")
PY
