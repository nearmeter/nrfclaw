#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path
import re
import sys

def fail(msg):
    print(f"FAIL {msg}")
    raise SystemExit(1)

def ok(msg):
    print(f"PASS {msg}")

ndp_path = Path("src/nrfclaw_ndp.c")
sub_path = Path("src/nrfclaw_ninalink_external_subscription.c")
cache_path = Path("src/nrfclaw_ninalink_state_cache.c")
cli_path = Path("NRFCLAW_CLI/nrfclaw_cli.py")
ble_path = Path("src/nrfclaw_ble.c")
hdr_path = Path("include/nrfclaw_ninalink_external_subscription.h")

for p in (ndp_path, sub_path, cache_path, cli_path, ble_path, hdr_path):
    if not p.exists():
        fail(f"missing required file: {p}")

ndp = ndp_path.read_text(encoding="utf-8")
sub = sub_path.read_text(encoding="utf-8")
cache = cache_path.read_text(encoding="utf-8")
cli = cli_path.read_text(encoding="utf-8")
ble = ble_path.read_text(encoding="utf-8")
hdr = hdr_path.read_text(encoding="utf-8")

# ------------------------------------------------------------------
# Isolate NDP_NINALINK_BRIDGE case robustly.
# ------------------------------------------------------------------
m_bridge = re.search(
    r"(?m)^(?P<indent>\s*)case\s+NDP_NINALINK_BRIDGE\s*:\s*\{",
    ndp,
)
if not m_bridge:
    fail("NDP_NINALINK_BRIDGE case missing")

m_link = re.search(
    r"(?m)^\s*case\s+NDP_NINALINK_LINK\s*:\s*\{",
    ndp[m_bridge.end():],
)
if not m_link:
    fail("NDP_NINALINK_LINK case missing after bridge case")

bridge_start = m_bridge.start()
bridge_end = m_bridge.end() + m_link.start()
block = ndp[bridge_start:bridge_end]

# ------------------------------------------------------------------
# Pre-switch physical/NUS-only filter.
# The entire opcode 0x5A must NOT be rejected before selector parsing.
# ------------------------------------------------------------------
m_owner = re.search(
    r"static\s+bool\s+nus_owner_key_opcode\s*\(\s*uint8_t\s+op\s*\)\s*"
    r"\{(?P<body>.*?)\}",
    ndp,
    re.S,
)
if not m_owner:
    fail("nus_owner_key_opcode() not found")

owner_body = m_owner.group("body")
if "NDP_NINALINK_BRIDGE" in owner_body:
    fail(
        "NDP_NINALINK_BRIDGE is still in nus_owner_key_opcode(); "
        "Application selectors 30..38 are rejected before the bridge case"
    )
ok("NDP_NINALINK_BRIDGE removed from global NUS-only opcode filter")

# ------------------------------------------------------------------
# Selector-level Application allowlist.
# ------------------------------------------------------------------
allow_re = re.compile(
    r"if\s*\(\s*enforce_auth\s*&&\s*"
    r"\(\s*n\s*<\s*1U\s*\|\|\s*p\s*\[\s*0\s*\]\s*<\s*30U\s*"
    r"\|\|\s*p\s*\[\s*0\s*\]\s*>\s*38U\s*\)\s*\)\s*"
    r"return\s+reply\s*\(\s*op\s*,\s*seq\s*,\s*NDP_FORBIDDEN",
    re.S,
)
if not allow_re.search(block):
    fail("selector-level Application allowlist 30..38 missing or malformed")
ok("Application plane allowlist is selectors 30..38 only")

# No blanket pre-selector FORBIDDEN may remain before selector 30.
selector30 = re.search(r"if\s*\(\s*p\s*\[\s*0\s*\]\s*==\s*30U\s*\)", block)
if not selector30:
    fail("selector 30 missing")
prefix = block[:selector30.start()]
blanket_re = re.compile(
    r"if\s*\(\s*enforce_auth\s*\)\s*"
    r"return\s+reply\s*\(\s*op\s*,\s*seq\s*,\s*NDP_FORBIDDEN",
    re.S,
)
if blanket_re.search(prefix):
    fail("blanket enforce_auth/FORBIDDEN remains before external selectors")
ok("no blanket pre-selector Application rejection remains")

# ------------------------------------------------------------------
# B5.4/B5.5 selectors must exist.
# ------------------------------------------------------------------
for selector in range(30, 40):
    if not re.search(
        rf"if\s*\(\s*p\s*\[\s*0\s*\]\s*==\s*{selector}U\s*\)",
        block,
    ):
        fail(f"bridge selector {selector} missing")
ok("selectors 30..39 present")

# B5.5 NDP reply payloads must stay <= 15 bytes.
expected_lengths = {36: 15, 37: 15, 38: 14}
for selector, expected in expected_lengths.items():
    m = re.search(
        rf"if\s*\(\s*p\s*\[\s*0\s*\]\s*==\s*{selector}U\s*\)",
        block,
    )
    if not m:
        fail(f"selector {selector} missing")
    later = []
    for s in range(selector + 1, 40):
        n = re.search(
            rf"if\s*\(\s*p\s*\[\s*0\s*\]\s*==\s*{s}U\s*\)",
            block[m.end():],
        )
        if n:
            later.append(m.end() + n.start())
    end = min(later) if later else len(block)
    part = block[m.start():end]
    reply_re = re.compile(
        rf"return\s+reply\s*\(\s*op\s*,\s*seq\s*,\s*NDP_OK\s*,"
        rf"\s*r\s*,\s*{expected}\s*,\s*out\s*,\s*ol\s*\)"
    )
    if not reply_re.search(part):
        fail(
            f"selector {selector} does not expose expected "
            f"{expected}-byte NDP_OK payload"
        )
ok("selectors 36..38 response payloads are 15/15/14 bytes")

# Selector 39 must remain Application-forbidden.
m39 = re.search(r"if\s*\(\s*p\s*\[\s*0\s*\]\s*==\s*39U\s*\)", block)
part39 = block[m39.start():] if m39 else ""
if not re.search(
    r"if\s*\(\s*enforce_auth\s*\).*?NDP_FORBIDDEN",
    part39,
    re.S,
):
    fail("selector 39 is not explicitly Application-forbidden/NUS-only")
ok("selector 39 handoff remains NUS-only")

# ------------------------------------------------------------------
# Async frame and transport independence.
# ------------------------------------------------------------------
if not re.search(
    r"#define\s+NRFCLAW_NINALINK_CHANGE_FRAME_SIZE\s+20U",
    hdr,
):
    fail("20-byte B5.5 async frame constant missing")
ok("20-byte asynchronous change frame defined")

if "nrfclaw_lora_" in sub or "schedule_response_for_uplink" in sub:
    fail("subscription engine is coupled directly to LoRa/radio internals")
ok("subscription engine has no LoRa/radio coupling")

if "mark_event_changed();" not in cache:
    fail("event revision hook missing from state cache")
if "mark_state_changed();" not in cache:
    fail("state revision hook missing from state cache")
ok("state/event revision hooks present")

# ------------------------------------------------------------------
# CLI demux/watch path.
# ------------------------------------------------------------------
if not re.search(
    r"len\s*\(\s*raw\s*\)\s*==\s*20\s+and\s+raw\s*\[\s*0\s*\]\s*==\s*0xE5",
    cli,
):
    fail("CLI does not demultiplex 20-byte 0xE5 B5.5 change frames")
if "run_ninalink_external_watch" not in cli:
    fail("CLI external watch command missing")
ok("CLI change-frame demux and external watch path present")

# ------------------------------------------------------------------
# Connection-scoped subscription lifecycle.
# ------------------------------------------------------------------
m_logout = re.search(r"(?m)^\s*case\s+NDP_AUTH_LOGOUT\s*:", ndp)
if not m_logout:
    fail("NDP_AUTH_LOGOUT case missing")
m_next = re.search(r"(?m)^\s*case\s+[A-Z0-9_]+\s*:", ndp[m_logout.end():])
logout_end = m_logout.end() + m_next.start() if m_next else len(ndp)
logout_block = ndp[m_logout.start():logout_end]
if "nrfclaw_ninalink_external_subscription_reset();" not in logout_block:
    fail("AUTH_LOGOUT does not clear B5.5 subscription")
ok("AUTH logout clears connection-scoped subscription")

if "nrfclaw_ble_programming_release" not in ble:
    fail("NUS->Application programming release primitive missing")
if "nrfclaw_ble_app_resume" not in ble:
    fail("BLE code has no Application resume path")
ok("NUS->Application handoff primitive and resume path present")

print("\nB5.5 source gate: PASS")
PY
