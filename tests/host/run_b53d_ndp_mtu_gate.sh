#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
from pathlib import Path
import re

ndp = Path("src/nrfclaw_ndp.c").read_text()
cli = Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text()

a = ndp.find("      case NDP_NINALINK_BRIDGE: {")
b = ndp.find("      case NDP_NINALINK_LINK: {", a)
if min(a,b) < 0:
    raise SystemExit("FAIL isolate NDP_NINALINK_BRIDGE")
block = ndp[a:b]

s24 = block.find("if(p[0]==24U)")
s25 = block.find("if(p[0]==25U)", s24)
if min(s24,s25) < 0:
    raise SystemExit("FAIL selector 24/25")
b24 = block[s24:s25]
if "return reply(op,seq,NDP_OK,r,13,out,ol);" not in b24:
    raise SystemExit("FAIL selector24 payload must remain 13 bytes")
if "return reply(op,seq,NDP_OK,r,20,out,ol);" in b24:
    raise SystemExit("FAIL selector24 still exceeds NDP-session payload budget")

s29 = block.find("if(p[0]==29U)")
if s29 < 0:
    raise SystemExit("FAIL selector29 session page missing")
tail = block[s29:]
if "return reply(op,seq,NDP_OK,r,7,out,ol);" not in tail:
    raise SystemExit("FAIL selector29 payload must be 7 bytes")

for m in re.finditer(r"reply\(op,seq,NDP_OK,r,([0-9]+),out,ol\)", block):
    n = int(m.group(1))
    if n > 15:
        raise SystemExit(f"FAIL bridge NDP reply payload {n} > 15")

if 'bytes([29]) + struct.pack("<I", node_id)' not in cli:
    raise SystemExit("FAIL CLI selector29 session read missing")

print("PASS selector24 cache summary payload=13")
print("PASS selector29 session metadata payload=7")
print("PASS all direct bridge NDP reply payloads <=15")
print("PASS CLI merges selector24 + selector29")
PY
