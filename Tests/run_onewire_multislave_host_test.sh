#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${TMPDIR:-/tmp}/onewire_multislave_step25_tests
mkdir -p "$OUT_DIR"

MASTER_FLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=1U"
SLAVE_FLAGS="-std=c11 -Wall -Wextra -Werror -O2 -DAPP_ONEWIRE_ROLE=2U -DAPP_ONEWIRE_SLAVE_ADDRESS=3U"

cc $MASTER_FLAGS \
  -I"$ROOT/Tests/stubs" -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire_protocol.c" \
  "$ROOT/Core/Src/app_onewire_master.c" \
  "$ROOT/Tests/test_app_onewire_master.c" \
  -o "$OUT_DIR/test_app_onewire_master"
"$OUT_DIR/test_app_onewire_master"

cc $SLAVE_FLAGS \
  -I"$ROOT/Tests/stubs" -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire_protocol.c" \
  "$ROOT/Core/Src/app_onewire_slave.c" \
  "$ROOT/Tests/test_app_onewire_slave.c" \
  -o "$OUT_DIR/test_app_onewire_slave_03"
"$OUT_DIR/test_app_onewire_slave_03"

cc $MASTER_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire.c" \
  "$ROOT/Tests/test_app_onewire_role.c" \
  -o "$OUT_DIR/test_app_onewire_role_master"
"$OUT_DIR/test_app_onewire_role_master"

cc $SLAVE_FLAGS \
  -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire.c" \
  "$ROOT/Tests/test_app_onewire_role.c" \
  -o "$OUT_DIR/test_app_onewire_role_slave_03"
"$OUT_DIR/test_app_onewire_role_slave_03"

cc $MASTER_FLAGS -ffunction-sections -fdata-sections \
  -I"$ROOT/Tests/stubs_app_uart" -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_uart.c" \
  "$ROOT/Tests/test_app_uart_onewire.c" \
  -Wl,--gc-sections -lm \
  -o "$OUT_DIR/test_app_uart_onewire"
"$OUT_DIR/test_app_uart_onewire"

cc -std=c11 -Wall -Wextra -Werror -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DAPP_ONEWIRE_ROLE=1U \
  -I"$ROOT/Tests/stubs" -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire_protocol.c" \
  "$ROOT/Core/Src/app_onewire_master.c" \
  "$ROOT/Tests/test_app_onewire_master.c" \
  -o "$OUT_DIR/test_app_onewire_master_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_onewire_master_san"

cc -std=c11 -Wall -Wextra -Werror -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DAPP_ONEWIRE_ROLE=2U -DAPP_ONEWIRE_SLAVE_ADDRESS=3U \
  -I"$ROOT/Tests/stubs" -I"$ROOT/Core/Inc" \
  "$ROOT/Core/Src/app_onewire_protocol.c" \
  "$ROOT/Core/Src/app_onewire_slave.c" \
  "$ROOT/Tests/test_app_onewire_slave.c" \
  -o "$OUT_DIR/test_app_onewire_slave_san"
ASAN_OPTIONS=detect_leaks=0 "$OUT_DIR/test_app_onewire_slave_san"

# Static invariants for the requested behavior.
grep -q 'master_state = APP_ONEWIRE_MASTER_IDLE' "$ROOT/Core/Src/app_onewire_master.c"
! grep -q 'APP_ONEWIRE_MASTER_BOOT.*HANDSHAKE_START' "$ROOT/Core/Src/app_onewire_master.c"
grep -q 'APP_ONEWIRE_MASTER_MAX_SLAVES' "$ROOT/Core/Inc/app_onewire_config.h"
grep -q 'ignored_foreign_frame_count' "$ROOT/Core/Src/app_onewire_slave.c"
grep -q 'len == 7U' "$ROOT/Core/Src/app_uart.c"
grep -q 'AppOneWire_SubmitTo' "$ROOT/Core/Src/app_uart.c"
grep -q 'SlaveAddress' "$ROOT/build.ps1"

echo "one-wire multi-slave on-demand step-25 host verification passed"
