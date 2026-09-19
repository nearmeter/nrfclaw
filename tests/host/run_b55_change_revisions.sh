#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"; OUT="${TMPDIR:-/tmp}/nrfclaw_b55_revisions"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_state_cache.c tests/host/test_b55_change_revisions.c -o "$OUT"
"$OUT"
