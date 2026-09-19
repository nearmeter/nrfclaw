#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw_b52_auto_discovery_test"
echo "[B5.2] host compiler: $($CC --version | head -1)"
echo "[B5.2] building automatic discovery tests"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_node_registry.c \
  src/nrfclaw_ninalink_auto_discovery.c \
  tests/host/test_ninalink_auto_discovery.c \
  -o "$OUT"
echo "[B5.2] running"
"$OUT"
