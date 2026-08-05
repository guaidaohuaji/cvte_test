#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/damper_boot_homing_step24_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=1U"

cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_damper" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_damper.c" \
  "$ROOT/Tests/test_app_damper_boot_homing.c" \
  -o "$OUT_DIR/test_app_damper_boot_homing"
"$OUT_DIR/test_app_damper_boot_homing"

cc $COMMON_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -I"$ROOT/Tests/stubs_damper" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_damper.c" \
  "$ROOT/Tests/test_app_damper_boot_homing.c" \
  -o "$OUT_DIR/test_app_damper_boot_homing_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_damper_boot_homing_san"

# Slave build must remain warning-clean and must not start the damper.
cc -std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs_damper" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_damper.c" \
  -o "$OUT_DIR/app_damper_slave.o"

grep -q 'DAMPER_STATE_BOOT_HOMING      = 0x08' "$ROOT/Core/Inc/app_damper.h"
grep -q 'APP_DAMPER_BOOT_HOMING_STEPS' "$ROOT/Core/Inc/app_damper_config.h"
grep -q 'state = DAMPER_STATE_BOOT_HOMING' "$ROOT/Core/Src/app_damper.c"

echo "damper boot-open homing step-24 host verification passed"
