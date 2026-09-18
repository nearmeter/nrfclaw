#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw-b48-command-registry-$$"
trap 'rm -f "$OUT"' EXIT

echo "[B4.8] host compiler: $($CC --version | head -1)"
echo "[B4.8] building command registry tests"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  tests/host/test_ninalink_command_registry.c \
  src/nrfclaw_ninalink_command_registry.c \
  -o "$OUT"
echo "[B4.8] running"
"$OUT"
