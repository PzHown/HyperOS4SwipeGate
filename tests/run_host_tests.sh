#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/swipegate-host-tests.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
cd "$ROOT"
CXX="${CXX:-clang++}"
FLAGS=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -g -fno-omit-frame-pointer)
if [[ "${SANITIZERS:-1}" == "1" ]]; then
    FLAGS+=(-fsanitize=address,undefined)
fi
"$CXX" "${FLAGS[@]}" -pthread -Inative/include \
    native/src/runtime_config_store.cpp tests/runtime_config_store_test.cpp -o "$BUILD/config_test"
"$CXX" "${FLAGS[@]}" -Inative/include tests/got_patch_transaction_test.cpp -o "$BUILD/got_test"
"$BUILD/got_test"
"$BUILD/config_test"
"$CXX" "${FLAGS[@]}" -DSWIPEGATE_VERSION_CODE=66 -Itests/stubs/native -Inative/include \
    -fsyntax-only native/src/control_channel.cpp native/src/config_reader.cpp native/src/hook_page_guard.cpp
mapfile -t STUBS < <(find tests/stubs/java -name '*.java' | sort)
PACKAGE=app/src/main/java/io/github/pzhown/hyperos4swipegate
javac --release 17 -d "$BUILD/classes" "${STUBS[@]}" \
    "$PACKAGE/ControlRequest.java" "$PACKAGE/SystemUiBridgeModule.java" \
    "$PACKAGE/NativeControlBridge.java" "$PACKAGE/ConfigBridge.java" tests/ControlChannelHostTest.java
java -cp "$BUILD/classes" io.github.pzhown.hyperos4swipegate.ControlChannelHostTest
