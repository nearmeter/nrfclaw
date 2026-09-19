#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY2'
from pathlib import Path
s=Path("src/nrfclaw_ninalink_bridge.c").read_text()
q=s.find("if (!queue_validated(")
r=s.find("remember_frame(wire);",q)
c=s.find("consume_validated_queue();",r)
a=s.find("if (ack_req) {",r)
if min(q,r,c,a)<0 or not(q<r<c<a):
    raise SystemExit("FAIL B5.3 ordering")
t=s.find("bool nrfclaw_ninalink_bridge_take(")
d=s.find("m_diag_queue[m_diag_tail]",t)
if min(t,d)<0:
    raise SystemExit("FAIL B5.3 diagnostic mirror")
print("PASS B5.3 queue admission -> remember -> consume -> ACK ordering")
print("PASS bridge_take reads diagnostic mirror")
PY2
