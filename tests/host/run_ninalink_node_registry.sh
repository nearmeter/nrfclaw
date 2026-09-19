#!/usr/bin/env bash
set -euo pipefail

CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw_b51_node_registry_test"

echo "[B5.1] host compiler: $($CC --version | head -1)"
echo "[B5.1] building node registry tests"

"$CC" \
  -std=c99 -Wall -Wextra -Werror \
  -Iinclude \
  src/nrfclaw_ninalink_node_registry.c \
  tests/host/test_ninalink_node_registry.c \
  -o "$OUT"

echo "[B5.1] running"
"$OUT"
