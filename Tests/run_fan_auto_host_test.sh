#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/fan_auto_step15_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Tests/test_app_auto_fan_profile.c" \
  -o "$OUT_DIR/test_app_auto_fan_profile"
"$OUT_DIR/test_app_auto_fan_profile"

cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_fan" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan.c" \
  "$ROOT/Tests/test_app_fan_startup.c" \
  -o "$OUT_DIR/test_app_fan_startup"
"$OUT_DIR/test_app_fan_startup"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_auto_control.c" \
  "$ROOT/Tests/test_app_auto_control_fan.c" \
  -o "$OUT_DIR/test_app_auto_control_fan"
"$OUT_DIR/test_app_auto_control_fan"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_auto_control.c" \
  -o "$OUT_DIR/app_auto_control_master.o"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_auto_control.c" \
  -o "$OUT_DIR/app_auto_control_slave.o"

echo "fan auto step-15 host verification passed"
