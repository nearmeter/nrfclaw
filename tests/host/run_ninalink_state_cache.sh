#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
OUT="${TMPDIR:-/tmp}/nrfclaw_b53_state_cache_test"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_state_cache.c tests/host/test_ninalink_state_cache.c \
  -o "$OUT"
"$OUT"
