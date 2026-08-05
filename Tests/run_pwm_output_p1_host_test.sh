#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/pwm_output_step21_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -I"$ROOT/Tests/stubs_pwm" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm.c" \
  "$ROOT/Tests/test_app_pwm_output.c" \
  -o "$OUT_DIR/test_app_pwm_output"
"$OUT_DIR/test_app_pwm_output"

cc $COMMON_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -I"$ROOT/Tests/stubs_pwm" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm.c" \
  "$ROOT/Tests/test_app_pwm_output.c" \
  -o "$OUT_DIR/test_app_pwm_output_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_pwm_output_san"

# Existing app_uart host test now compiles against the atomic PWM API.
cc -std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_uart.c" \
  "$ROOT/Tests/test_app_uart_fan_manual.c" \
  -o "$OUT_DIR/test_app_uart_regression"
"$OUT_DIR/test_app_uart_regression"

echo "general PWM output step-21 phase-P1 host verification passed"
