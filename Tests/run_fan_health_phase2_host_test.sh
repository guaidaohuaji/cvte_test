#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/fan_health_step19_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=1U"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Tests/test_app_auto_fan_profile.c" \
  -o "$OUT_DIR/test_app_auto_fan_profile"
"$OUT_DIR/test_app_auto_fan_profile"


cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_fan" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan.c" \
  "$ROOT/Tests/test_app_fan_startup.c" \
  -o "$OUT_DIR/test_app_fan_safety"
"$OUT_DIR/test_app_fan_safety"

cc $COMMON_FLAGS \
  -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -I"$ROOT/Tests/stubs_fan" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan.c" \
  "$ROOT/Tests/test_app_fan_startup.c" \
  -o "$OUT_DIR/test_app_fan_safety_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_fan_safety_san"

cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_health.c" \
  "$ROOT/Tests/test_app_fan_health.c" \
  -o "$OUT_DIR/test_app_fan_health"
"$OUT_DIR/test_app_fan_health"

cc $COMMON_FLAGS \
  -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_health.c" \
  "$ROOT/Tests/test_app_fan_health.c" \
  -o "$OUT_DIR/test_app_fan_health_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_fan_health_san"

cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_fan_health.c" \
  -o "$OUT_DIR/app_fan_health_master.o"

cc -std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_fan_health.c" \
  -o "$OUT_DIR/app_fan_health_slave.o"

echo "fan health step-19 phase-2 host verification passed"
