#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[B3.2] host compiler: $("$CC" --version | head -n1)"
echo "[B3.2] building NinaLink core + semantic message tests"

"$CC" \
    -std=c99 \
    -Wall -Wextra -Werror -pedantic \
    -ffunction-sections -fdata-sections \
    -I"$ROOT/include" \
    "$ROOT/src/nrfclaw_ninalink.c" \
    "$ROOT/src/nrfclaw_ninalink_msg.c" \
    "$ROOT/tests/host/test_ninalink_messages.c" \
    -Wl,--gc-sections \
    -o "$TMP/test_ninalink_messages"

echo "[B3.2] running"
"$TMP/test_ninalink_messages"
