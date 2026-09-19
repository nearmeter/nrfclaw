#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY2'
from pathlib import Path
ext=Path("src/nrfclaw_ninalink_external.c").read_text(); ndp=Path("src/nrfclaw_ndp.c").read_text(); cli=Path("NRFCLAW_CLI/nrfclaw_cli.py").read_text(); cache=Path("src/nrfclaw_ninalink_state_cache.c").read_text()
checks=[("schema","NRFCLAW_NINALINK_EXTERNAL_SCHEMA_VERSION 1U" in Path("include/nrfclaw_ninalink_external.h").read_text()),("cursor","nrfclaw_ninalink_event_history_get_after" in cache),("snapshot","ninalink_external_snapshot" in cli),("events","ninalink_external_events" in cli),("unknown raw","!nrfclaw_capability_descriptor" in ext)]
for n,o in checks:
    if not o: raise SystemExit("FAIL "+n)
    print("PASS "+n)
a=ndp.find("      case NDP_NINALINK_BRIDGE: {");b=ndp.find("      case NDP_NINALINK_LINK: {",a);block=ndp[a:b]
for sel,size in ((30,12),(31,15),(32,15),(33,15),(34,7),(35,6)):
    s=block.find(f"if(p[0]=={sel}U)")
    if s<0: raise SystemExit(f"FAIL selector {sel}")
    e=min([x for x in [block.find(f"if(p[0]=={q}U)",s+1) for q in range(sel+1,36)] if x>=0] or [len(block)])
    if f"return reply(op,seq,NDP_OK,r,{size},out,ol);" not in block[s:e]: raise SystemExit(f"FAIL selector {sel} payload")
for bad in ("nrfclaw_lora_","schedule_response_for_uplink","send_async"):
    if bad in ext: raise SystemExit("FAIL radio coupling "+bad)
print("PASS selectors 30..35 payloads <=15")
print("PASS external facade has no radio operations")
PY2
