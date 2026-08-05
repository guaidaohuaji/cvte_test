#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/ntc_lookup_step27_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Tests/test_app_ntc_lookup.c" \
  -o "$OUT_DIR/test_app_ntc_lookup"
"$OUT_DIR/test_app_ntc_lookup"

echo "NTC lookup Step27 host verification passed"
