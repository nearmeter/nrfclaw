#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw_b53c_sequence_semantics"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_state_cache.c \
  tests/host/test_b53c_sequence_semantics.c \
  -o "$OUT"
"$OUT"
