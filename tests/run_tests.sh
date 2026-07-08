#!/bin/bash
# penguinC test runner
# Usage: ./tests/run_tests.sh [--no-valgrind] [test_number ...]
# Compiles .pc files, links with runtime, runs, and compares output to .expected
# Runs valgrind on compiled programs by default to check for memory leaks

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BIN="$PROJECT_DIR/penguinc"
STDLIB_DIR="$PROJECT_DIR/stdlib"
RUNTIME_DIR="$PROJECT_DIR/runtime"
TESTS_DIR="$SCRIPT_DIR/runtime"
TMPDIR=$(mktemp -d)

USE_VALGRIND=1
TESTS=()

for arg in "$@"; do
    case "$arg" in
        --no-valgrind) USE_VALGRIND=0 ;;
        *)             TESTS+=("$arg") ;;
    esac
done

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

if [ ! -f "$BIN" ]; then
    echo "ERROR: penguinc binary not found. Run 'make' first."
    exit 1
fi

run_test() {
    local test_num="$1"
    local result_dir="$2"

    local pc_file
    pc_file=$(ls "$TESTS_DIR"/test_$(printf '%02d' $test_num)_*.pc 2>/dev/null | head -1)
    local expected_file
    expected_file=$(ls "$TESTS_DIR"/test_$(printf '%02d' $test_num)_*.expected 2>/dev/null | head -1)

    if [ -z "$pc_file" ] || [ -z "$expected_file" ]; then
        echo "SKIP" > "$result_dir/status"
        echo "test_$(printf '%02d' $test_num)" > "$result_dir/name"
        echo "files not found" > "$result_dir/detail"
        return
    fi

    local name
    name=$(basename "$pc_file" .pc)
    local exe="$result_dir/$name"
    echo "$name" > "$result_dir/name"

    export STDLIB="$STDLIB_DIR"
    export RUNTIME="$RUNTIME_DIR"
    if ! "$BIN" -o "$exe" "$pc_file" 2>/dev/null; then
        echo "FAIL" > "$result_dir/status"
        echo "compilation failed" > "$result_dir/detail"
        return
    fi

    local expected
    expected=$(cat "$expected_file")

    # Run without valgrind first to check output
    local actual
    actual=$("$exe" 2>/dev/null) || true

    if [ "$actual" != "$expected" ]; then
        echo "FAIL" > "$result_dir/status"
        echo "output mismatch" > "$result_dir/detail"
        echo "expected: $(echo "$expected" | head -1)" > "$result_dir/expected"
        echo "got: $(echo "$actual" | head -1)" > "$result_dir/got"
        return
    fi

    # Run with valgrind to check for memory leaks in the compiled program
    if [ "$USE_VALGRIND" -eq 1 ]; then
        local vg_out="$result_dir/valgrind.log"
        valgrind --leak-check=full --errors-for-leak-kinds=definite \
            --error-exitcode=99 --log-file="$vg_out" \
            "$exe" >/dev/null 2>&1
        local vg_exit=$?

        if [ "$vg_exit" -eq 99 ]; then
            local lost
            lost=$(grep "definitely lost:" "$vg_out" | head -1)
            echo "LEAK" > "$result_dir/status"
            echo "$lost" > "$result_dir/detail"
            return
        fi

        if [ "$vg_exit" -ne 0 ] && [ -s "$vg_out" ]; then
            echo "VGERR" > "$result_dir/status"
            echo "valgrind error (exit $vg_exit)" > "$result_dir/detail"
            return
        fi
    fi

    echo "PASS" > "$result_dir/status"
}

export -f run_test
export TESTS_DIR BIN STDLIB_DIR RUNTIME_DIR USE_VALGRIND

echo "=== penguinC Runtime Tests ==="
echo ""

if [ ${#TESTS[@]} -gt 0 ]; then
    SEQ="${TESTS[*]}"
else
    SEQ=$(seq 1 39)
fi

PIDS=()
for t in $SEQ; do
    result_dir="$TMPDIR/test_$(printf '%03d' $t)"
    mkdir -p "$result_dir"
    run_test "$t" "$result_dir" &
    PIDS+=($!)
done

wait "${PIDS[@]}" 2>/dev/null

PASS=0
FAIL=0
SKIP=0

for t in $SEQ; do
    result_dir="$TMPDIR/test_$(printf '%03d' $t)"
    [ -f "$result_dir/status" ] || continue

    status=$(cat "$result_dir/status")
    name=$(cat "$result_dir/name" 2>/dev/null || echo "test_$(printf '%02d' $t)")

    case "$status" in
        PASS)
            echo "  PASS: $name"
            PASS=$((PASS + 1))
            ;;
        FAIL)
            echo "  FAIL: $name ($(cat "$result_dir/detail"))"
            [ -f "$result_dir/expected" ] && echo "    Expected: $(cat "$result_dir/expected" | sed 's/^expected: //')"
            [ -f "$result_dir/got" ]      && echo "    Got:      $(cat "$result_dir/got" | sed 's/^got: //')"
            FAIL=$((FAIL + 1))
            ;;
        LEAK)
            echo "  LEAK: $name"
            echo "    $(cat "$result_dir/detail")"
            FAIL=$((FAIL + 1))
            ;;
        VGERR)
            echo "  VGERR: $name ($(cat "$result_dir/detail"))"
            FAIL=$((FAIL + 1))
            ;;
        SKIP)
            echo "  SKIP: $name ($(cat "$result_dir/detail"))"
            SKIP=$((SKIP + 1))
            ;;
    esac
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
