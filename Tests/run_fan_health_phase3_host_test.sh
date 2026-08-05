#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/fan_health_step20_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=1U"

# Re-run the formal shutdown and safety-lock tests from Step19.
"$ROOT/Tests/run_fan_health_phase2_host_test.sh"

# W2 object 0x06: legacy query, V1 query, health V2 query and clear-fault command.
cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_uart.c" \
  "$ROOT/Tests/test_app_uart_fan_manual.c" \
  -o "$OUT_DIR/test_app_uart_fan_health_w2"
"$OUT_DIR/test_app_uart_fan_health_w2"

cc $COMMON_FLAGS \
  -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_uart.c" \
  "$ROOT/Tests/test_app_uart_fan_manual.c" \
  -o "$OUT_DIR/test_app_uart_fan_health_w2_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_uart_fan_health_w2_san"

# Strict compile for both one-wire roles.
cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_uart.c" \
  -o "$OUT_DIR/app_uart_master.o"

cc -std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_uart.c" \
  -o "$OUT_DIR/app_uart_slave.o"

echo "fan health step-20 phase-3 W2 verification passed"
