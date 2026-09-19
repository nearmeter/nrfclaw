#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"; OUT="${TMPDIR:-/tmp}/nrfclaw_b54_event_cursor"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude src/nrfclaw_ninalink_state_cache.c tests/host/test_b54_event_cursor.c -o "$OUT"
"$OUT"
