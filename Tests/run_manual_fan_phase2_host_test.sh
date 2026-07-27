#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/manual_fan_phase2_tests
mkdir -p "$OUT_DIR"

COMMON_FLAGS="-std=c11 -Wall -Wextra -Werror -O2"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_uart.c" \
  "$ROOT/Tests/test_app_uart_fan_manual.c" \
  -o "$OUT_DIR/test_app_uart_fan_manual"
"$OUT_DIR/test_app_uart_fan_manual"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_uart.c" \
  -o "$OUT_DIR/app_uart_master.o"

cc $COMMON_FLAGS \
  -DAPP_ONEWIRE_ROLE=2U \
  -I"$ROOT/Tests/stubs_app_uart" \
  -I"$ROOT/Core/Inc" \
  -c "$ROOT/Core/Src/app_uart.c" \
  -o "$OUT_DIR/app_uart_slave.o"

"$ROOT/Tests/run_manual_fan_phase1_host_test.sh"
"$ROOT/Tests/run_fan_auto_host_test.sh"
"$ROOT/Tests/run_fan_bpf_host_test.sh"

echo "manual fan RPM controller phase-2 host verification passed"
