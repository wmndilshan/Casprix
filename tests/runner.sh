#!/usr/bin/env bash
# Casprix test runner — runs all compiler .cpx tests
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
CASPRIX="${ROOT_DIR}/build/casprix"
TESTS_DIR="${SCRIPT_DIR}/compiler"
PASS=0; FAIL=0; SKIP=0

if [[ ! -x "$CASPRIX" ]]; then
    echo "ERROR: casprix not found at $CASPRIX — run cmake build first"
    exit 1
fi

echo "=== Casprix Compiler Test Suite ==="
echo "Binary : $CASPRIX"
echo "Tests  : $TESTS_DIR"
echo ""

for cpx in "$TESTS_DIR"/test_*.cpx; do
    name="$(basename "$cpx" .cpx)"
    if "$CASPRIX" "$cpx" >/dev/null 2>&1; then
        echo "  PASS  $name"
        ((PASS++))
    else
        echo "  FAIL  $name"
        ((FAIL++))
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[[ $FAIL -eq 0 ]]
