#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw_b53d_session_epoch"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_state_cache.c \
  tests/host/test_b53d_session_epoch.c \
  -o "$OUT"
"$OUT"
