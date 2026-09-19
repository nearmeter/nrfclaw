#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"; OUT="${TMPDIR:-/tmp}/nrfclaw_b55_subscription"
"$CC" -std=c99 -Wall -Wextra -Werror -Iinclude \
  src/nrfclaw_ninalink_external_subscription.c tests/host/test_b55_subscription.c -o "$OUT"
"$OUT"
