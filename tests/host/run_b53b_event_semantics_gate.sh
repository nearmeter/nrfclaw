#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY2'
from pathlib import Path
s=Path("src/nrfclaw_ninalink_state_cache.c").read_text()
e=s.find("if (message_type == NRFCLAW_NINALINK_MSG_CAP_EVENT)")
p=s.find("for (i = 0U; i < entry_count; i++) {", e)
r=s.find("return true;", e)
if min(e,r)<0 or (p>=0 and p<r):
    raise SystemExit("FAIL: CAP_EVENT reaches persistent state upsert")
if "push_event(node_id, sequence, &entries[i], now_s);" not in s[e:r]:
    raise SystemExit("FAIL: CAP_EVENT is not journaled")
link=Path("src/nrfclaw_ninalink_link.c").read_text()
if "NRFCLAW_NINALINK_MSG_CAP_EVENT" not in link or "build_event_test" not in link:
    raise SystemExit("FAIL: synthetic node CAP_EVENT emitter missing")
print("PASS CAP_EVENT -> event history -> return before persistent state upsert")
print("PASS deterministic node CAP_EVENT emitter present")
ndp=Path("src/nrfclaw_ndp.c").read_text()
if "if(p[0]==12U) {\n            uint8_t event_kind;" not in ndp:
    raise SystemExit("FAIL: B5.3b synthetic event is not on LINK selector 12")
cli=Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()
if "bytes([12, kinds[event_name]])" not in cli:
    raise SystemExit("FAIL: CLI does not use LINK selector 12 for CAP_EVENT test")
print("PASS B5.3b LINK selector 12 is consistent in firmware + CLI")
PY2
