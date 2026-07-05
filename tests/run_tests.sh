#!/bin/bash
# penguinC test runner
# Usage: ./tests/run_tests.sh [test_number]
# Compiles .pc files, links with runtime, runs, and compares output to .expected

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_DIR/penguinc"
STDLIB_DIR="$PROJECT_DIR/stdlib"
RUNTIME_DIR="$PROJECT_DIR/runtime"
TMPDIR=$(mktemp -d)

PASS=0
FAIL=0
SKIP=0

cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

if [ ! -f "$BIN" ]; then
    echo "ERROR: penguinc binary not found. Run 'make' first."
    exit 1
fi

run_test() {
    local test_num="$1"
    local pc_file="$SCRIPT_DIR/test_$(printf '%02d' $test_num)_*.pc"
    local expected_file="$SCRIPT_DIR/test_$(printf '%02d' $test_num)_*.expected"

    # Resolve globs
    pc_file=$(ls $pc_file 2>/dev/null | head -1)
    expected_file=$(ls $expected_file 2>/dev/null | head -1)

    if [ -z "$pc_file" ] || [ -z "$expected_file" ]; then
        echo "  SKIP: test_$(printf '%02d' $test_num) (files not found)"
        SKIP=$((SKIP + 1))
        return
    fi

    local name=$(basename "$pc_file" .pc)
    local exe="$TMPDIR/$name"

    # Compile and link (compiler resolves imports and links automatically)
    export STDLIB="$STDLIB_DIR"
    export RUNTIME="$RUNTIME_DIR"
    if ! "$BIN" -o "$exe" "$pc_file" 2>/dev/null; then
        echo "  FAIL: $name (compilation failed)"
        FAIL=$((FAIL + 1))
        return
    fi

    # Run and capture output
    local actual
    actual=$("$exe" 2>/dev/null) || true

    # Compare
    local expected
    expected=$(cat "$expected_file")

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    Expected: $(echo "$expected" | head -1)"
        echo "    Got:      $(echo "$actual" | head -1)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== penguinC Test ==="
echo ""

if [ $# -gt 0 ]; then
    # Run specific test
    for t in "$@"; do
        run_test "$t"
    done
else
    # Run all tests
    for t in $(seq 1 37); do
        run_test "$t" 2>/dev/null
    done
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
