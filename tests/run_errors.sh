#!/bin/bash
# penguinC error test runner
# Usage: ./tests/run_errors.sh [category ...]
# Tests that the compiler correctly rejects invalid code with expected error messages.
# Categories: typecheck mutability thread_safety other
# Uses --for-test flag for clean, machine-readable error output.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_DIR/penguinc"
STDLIB_DIR="$PROJECT_DIR/stdlib"
RUNTIME_DIR="$PROJECT_DIR/runtime"
ERRORS_DIR="$SCRIPT_DIR/errors"
TMPDIR=$(mktemp -d)

CATEGORIES=()

for arg in "$@"; do
    case "$arg" in
        --no-valgrind) ;;  # ignored for error tests
        *)             CATEGORIES+=("$arg") ;;
    esac
done

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

if [ ! -f "$BIN" ]; then
    echo "ERROR: penguinc binary not found. Run 'make' first."
    exit 1
fi

if [ ${#CATEGORIES[@]} -eq 0 ]; then
    CATEGORIES=(typecheck mutability thread_safety other)
fi

run_error_test() {
    local pc_file="$1"
    local error_file="$2"
    local result_dir="$3"

    local name
    name=$(basename "$pc_file" .pc)
    echo "$name" > "$result_dir/name"

    local expected
    expected=$(cat "$error_file")

    export STDLIB="$STDLIB_DIR"
    export RUNTIME="$RUNTIME_DIR"

    # Capture stderr (compiler output) — compilation should FAIL
    local actual
    actual=$("$BIN" --for-test -o "$TMPDIR/test_out" "$pc_file" 2>&1) || true

    if [ -z "$actual" ]; then
        echo "FAIL" > "$result_dir/status"
        echo "no error output (compilation succeeded?)" > "$result_dir/detail"
        return
    fi

    # Check that the expected error message appears in the output
    if echo "$actual" | grep -qF "$expected"; then
        echo "PASS" > "$result_dir/status"
    else
        echo "FAIL" > "$result_dir/status"
        echo "error message mismatch" > "$result_dir/detail"
        echo "expected substring: $expected" > "$result_dir/expected"
        echo "actual output: $(echo "$actual" | head -1)" > "$result_dir/got"
    fi
}

export -f run_error_test
export BIN STDLIB_DIR RUNTIME_DIR TMPDIR

echo "=== penguinC Error Tests ==="
echo ""

TEST_NUM=0
PIDS=()

for cat in "${CATEGORIES[@]}"; do
    cat_dir="$ERRORS_DIR/$cat"
    [ -d "$cat_dir" ] || continue

    for pc_file in "$cat_dir"/*.pc; do
        [ -f "$pc_file" ] || continue
        error_file="${pc_file%.pc}.error"
        [ -f "$error_file" ] || continue

        TEST_NUM=$((TEST_NUM + 1))
        result_dir="$TMPDIR/err_$(printf '%03d' $TEST_NUM)"
        mkdir -p "$result_dir"
        run_error_test "$pc_file" "$error_file" "$result_dir" &
        PIDS+=($!)
    done
done

wait "${PIDS[@]}" 2>/dev/null

PASS=0
FAIL=0

for t in $(seq 1 $TEST_NUM); do
    result_dir="$TMPDIR/err_$(printf '%03d' $t)"
    [ -f "$result_dir/status" ] || continue

    status=$(cat "$result_dir/status")
    name=$(cat "$result_dir/name" 2>/dev/null || echo "test_$t")

    case "$status" in
        PASS)
            echo "  PASS: $name"
            PASS=$((PASS + 1))
            ;;
        FAIL)
            echo "  FAIL: $name ($(cat "$result_dir/detail"))"
            [ -f "$result_dir/expected" ] && echo "    Expected: $(cat "$result_dir/expected")"
            [ -f "$result_dir/got" ]      && echo "    Got:      $(cat "$result_dir/got")"
            FAIL=$((FAIL + 1))
            ;;
    esac
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
