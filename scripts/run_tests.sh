#!/usr/bin/env bash
# scripts/run_tests.sh — Run the Casprix test suite
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

# Find compiler
CASPRIX=""
if [ -f "$BUILD_DIR/casprix" ]; then
    CASPRIX="$BUILD_DIR/casprix"
elif [ -f "$BUILD_DIR/bin/casprix" ]; then
    CASPRIX="$BUILD_DIR/bin/casprix"
elif command -v casprix &>/dev/null; then
    CASPRIX="casprix"
else
    echo "ERROR: casprix not found. Run scripts/build.sh first." >&2
    exit 1
fi

echo "=== Casprix Compiler Test Suite ==="
echo "  Compiler: $CASPRIX"
echo "  Tests:    $PROJECT_ROOT/tests/compiler/"
echo ""

PASS=0
FAIL=0
SKIP=0

run_test() {
    local file="$1"
    local name
    name="$(basename "$file" .cpx)"

    if "$CASPRIX" "$file" --parse-only >/dev/null 2>&1; then
        echo "  [PASS] $name"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $name"
        FAIL=$((FAIL + 1))
    fi
}

for f in "$PROJECT_ROOT/tests/compiler/"*.cpx; do
    [ -f "$f" ] || continue
    run_test "$f"
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ]
