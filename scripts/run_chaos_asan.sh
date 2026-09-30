#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ASAN_DIR="$ROOT_DIR/.build-asan"

echo "=== [1/4] Configuring & Building aigate with Sanitizers (ASan/UBSan) ==="
export no_proxy="127.0.0.1,localhost,${no_proxy:-}"
export NO_PROXY="127.0.0.1,localhost,${NO_PROXY:-}"
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY 2>/dev/null || true

cmake -B "$BUILD_ASAN_DIR" \
    -DAIGATE_SANITIZERS=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -S "$ROOT_DIR"

cmake --build "$BUILD_ASAN_DIR" --target aigate aigate_unit_tests -j"$(nproc)"

echo "=== [2/4] Running CTest Unit Tests under ASan/UBSan ==="
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1:halt_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

ctest --test-dir "$BUILD_ASAN_DIR" --output-on-failure

echo "=== [3/4] Running End-to-End Chaos Suite under ASan/UBSan ==="
python3 "$ROOT_DIR/tests/chaos/test_chaos.py" \
    --bin "$BUILD_ASAN_DIR/aigate" \
    --gateway-port 18089 \
    --chaos-port 19099

echo "=== [4/4] Sanitizer & Chaos Verification PASSED (0 Leaks, 0 Errors) ==="
