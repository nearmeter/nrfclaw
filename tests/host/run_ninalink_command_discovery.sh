#!/usr/bin/env bash
set -euo pipefail

CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw-b49-command-discovery-$$"
trap 'rm -f "$OUT"' EXIT

echo "[B4.9] host compiler: $($CC --version | head -1)"
echo "[B4.9] building command discovery tests"

"$CC" \
  -std=c99 \
  -Wall -Wextra -Werror \
  -Iinclude \
  tests/host/test_ninalink_command_discovery.c \
  src/nrfclaw_ninalink_command_registry.c \
  src/nrfclaw_ninalink_command_discovery.c \
  -o "$OUT"

echo "[B4.9] running"
"$OUT"
