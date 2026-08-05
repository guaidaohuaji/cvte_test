#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/pwm_input_step22_tests
mkdir -p "$OUT_DIR"
COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -DAPP_PWM_INPUT_ENGINE_MODE=APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY \
  -I"$ROOT/Tests/stubs_pwm_input" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm_input.c" \
  "$ROOT/Tests/test_app_pwm_input.c" \
  -o "$OUT_DIR/test_app_pwm_input"
"$OUT_DIR/test_app_pwm_input"

cc $COMMON_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -DAPP_PWM_INPUT_ENGINE_MODE=APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY \
  -I"$ROOT/Tests/stubs_pwm_input" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm_input.c" \
  "$ROOT/Tests/test_app_pwm_input.c" \
  -o "$OUT_DIR/test_app_pwm_input_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_pwm_input_san"

echo "general PWM input step-22 phase-P2 host verification passed"
