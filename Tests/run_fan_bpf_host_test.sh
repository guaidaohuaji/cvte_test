#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/fan_bpf_stage3_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_feedback_bpf.c" \
  "$ROOT/Tests/test_app_fan_feedback_bpf.c" \
  -lm -o "$OUT_DIR/test_app_fan_feedback_bpf"
"$OUT_DIR/test_app_fan_feedback_bpf"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_feedback_adc.c" \
  "$ROOT/Tests/test_app_fan_feedback_reset.c" \
  -o "$OUT_DIR/test_app_fan_feedback_reset"
"$OUT_DIR/test_app_fan_feedback_reset"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_feedback_adc.c" \
  "$ROOT/Tests/test_app_fan_feedback_source.c" \
  -o "$OUT_DIR/test_app_fan_feedback_source"
"$OUT_DIR/test_app_fan_feedback_source"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_fan_feedback_adc.c" \
  "$ROOT/Core/Src/app_fan_feedback_bpf.c" \
  "$ROOT/Tests/test_app_fan_feedback_integration.c" \
  -lm -o "$OUT_DIR/test_app_fan_feedback_integration"
"$OUT_DIR/test_app_fan_feedback_integration"

# Verify the one-line rollback mode still compiles and selects the legacy path.
cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  -DAPP_FAN_FEEDBACK_OUTPUT_MODE=0U \
  "$ROOT/Core/Src/app_fan_feedback_adc.c" \
  "$ROOT/Tests/test_app_fan_feedback_source.c" \
  -o "$OUT_DIR/test_app_fan_feedback_source_legacy"
"$OUT_DIR/test_app_fan_feedback_source_legacy"

# Verify BPF-primary operation without legacy fallback.
cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  -DAPP_FAN_FEEDBACK_LEGACY_FALLBACK_ENABLE=0U \
  "$ROOT/Core/Src/app_fan_feedback_adc.c" \
  "$ROOT/Tests/test_app_fan_feedback_source.c" \
  -o "$OUT_DIR/test_app_fan_feedback_source_no_fallback"
"$OUT_DIR/test_app_fan_feedback_source_no_fallback"

# Compile-only checks for both optional filter feature switches.  BPF-primary
# output is intentionally disabled in these configurations.
cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  -DAPP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE=0 \
  -c "$ROOT/Core/Src/app_fan_feedback_bpf.c" \
  -o "$OUT_DIR/app_fan_feedback_bpf_no_tach.o"

cc $COMMON_FLAGS \
  -I"$ROOT/Core/Inc" \
  -DAPP_FAN_FEEDBACK_BPF_SHADOW_ENABLE=0 \
  -DAPP_FAN_FEEDBACK_BPF_TACH_SHADOW_ENABLE=0 \
  -c "$ROOT/Core/Src/app_fan_feedback_bpf.c" \
  -o "$OUT_DIR/app_fan_feedback_bpf_disabled.o"

echo "fan BPF stage-3 host verification passed"
