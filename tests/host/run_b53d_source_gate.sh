#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY2'
from pathlib import Path

link=Path("src/nrfclaw_ninalink_link.c").read_text()
bridge=Path("src/nrfclaw_ninalink_bridge.c").read_text()
cache=Path("src/nrfclaw_ninalink_state_cache.c").read_text()
ndp=Path("src/nrfclaw_ndp.c").read_text()
cli=Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()

checks = [
    ("SoftDevice RNG session", "sd_rand_application_vector_get" in link),
    ("session metadata appended", "NRFCLAW_NINALINK_META_SESSION_ID" in link and "build_values_with_session" in link),
    ("normal report sessionized", "build_values_with_session(\n            &frame,\n            NRFCLAW_NINALINK_MSG_CAP_REPORT" in link),
    ("event sessionized", "NRFCLAW_NINALINK_MSG_CAP_EVENT" in link and "build_values_with_session" in link),
    ("bridge extracts session before dedup", bridge.find("extract_session_metadata(&frame") < bridge.find("!session_changed && is_duplicate(wire)")),
    ("session commit after admission", bridge.find("if (!queue_validated(") < bridge.find("commit_session(frame.node_id") < bridge.find("remember_frame(wire);")),
    ("consumer strips metadata", "session_id = entries[src_i].value.v.u32" in bridge and "dst_i" in bridge),
    ("cache epoch reset", "node->pub.session_changes" in cache and "memset(node->values" in cache),
    ("NDP selector14", "if(p[0]==14U)" in ndp and "nrfclaw_ninalink_link_force_session" in ndp),
    ("CLI session commands", "ninalink-session-status" in cli and "ninalink-session-force" in cli),
]
for name, ok in checks:
    if not ok:
        raise SystemExit(f"FAIL {name}")
    print(f"PASS {name}")
PY2
