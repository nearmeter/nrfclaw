#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw-b410-capability-discovery-$$"
trap 'rm -f "$OUT"' EXIT
echo "[B4.10] host compiler: $($CC --version | head -1)"
echo "[B4.10] building capability discovery tests"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  tests/host/test_ninalink_capability_discovery.c \
  src/nrfclaw_ninalink_capability_discovery.c \
  -o "$OUT"
echo "[B4.10] running"
"$OUT"
