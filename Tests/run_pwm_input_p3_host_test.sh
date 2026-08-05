#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/pwm_input_step23_tests
mkdir -p "$OUT_DIR"
COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

# Step23 default DMA autorange engine.
cc $COMMON_FLAGS \
  -DAPP_PWM_INPUT_HOST_TEST=1 \
  -I"$ROOT/Tests/stubs_pwm_input" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm_input.c" \
  "$ROOT/Tests/test_app_pwm_input_dma.c" \
  -o "$OUT_DIR/test_app_pwm_input_dma"
"$OUT_DIR/test_app_pwm_input_dma"

cc $COMMON_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -O1 \
  -DAPP_PWM_INPUT_HOST_TEST=1 \
  -I"$ROOT/Tests/stubs_pwm_input" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm_input.c" \
  "$ROOT/Tests/test_app_pwm_input_dma.c" \
  -o "$OUT_DIR/test_app_pwm_input_dma_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_pwm_input_dma_san"

# One-line rollback engine remains buildable and preserves Step22 semantics.
cc $COMMON_FLAGS \
  -DAPP_PWM_INPUT_ENGINE_MODE=APP_PWM_INPUT_ENGINE_INTERRUPT_LEGACY \
  -I"$ROOT/Tests/stubs_pwm_input" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_pwm_input.c" \
  "$ROOT/Tests/test_app_pwm_input.c" \
  -o "$OUT_DIR/test_app_pwm_input_legacy"
"$OUT_DIR/test_app_pwm_input_legacy"

echo "general PWM input step-23 phase-P3 host verification passed"
