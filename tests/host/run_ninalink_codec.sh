#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
CC="${CC:-cc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "[B3.1] host compiler: $("$CC" --version | head -n1)"
echo "[B3.1] building pure NinaLink codec tests"

"$CC" \
    -std=c99 \
    -Wall -Wextra -Werror -pedantic \
    -I"$ROOT/include" \
    "$ROOT/src/nrfclaw_ninalink.c" \
    "$ROOT/tests/host/test_ninalink_codec.c" \
    -o "$TMP/test_ninalink_codec"

echo "[B3.1] running"
"$TMP/test_ninalink_codec"
