#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"; OUT="${TMPDIR:-/tmp}/nrfclaw_b54_external"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude src/nrfclaw_ninalink_node_registry.c src/nrfclaw_ninalink_state_cache.c src/nrfclaw_ninalink_external.c tests/host/test_b54_external.c -o "$OUT"
"$OUT"
