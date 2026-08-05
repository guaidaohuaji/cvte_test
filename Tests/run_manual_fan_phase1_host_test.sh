#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/manual_fan_phase1_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_manual_fan_control.c" \
  "$ROOT/Tests/test_app_manual_fan_control.c" \
  -o "$OUT_DIR/test_app_manual_fan_control"
"$OUT_DIR/test_app_manual_fan_control"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_manual_fan_control.c" \
  -o "$OUT_DIR/app_manual_fan_control_master.o"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_manual_fan_control.c" \
  -o "$OUT_DIR/app_manual_fan_control_slave.o"

echo "manual fan RPM controller phase-1 host verification passed"
