#!/usr/bin/env bash
# scripts/build.sh — Build Casprix on Unix/Linux/macOS
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"

# Parse arguments
BUILD_TYPE="${1:-Release}"
JOBS="${2:-$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)}"

echo "=== Casprix Build ==="
echo "  Project:    $PROJECT_ROOT"
echo "  Build type: $BUILD_TYPE"
echo "  Jobs:       $JOBS"
echo ""

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_COMPILER=ON \
    -DBUILD_RUNTIME=ON \
    -DBUILD_TESTS=ON

cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS"

echo ""
echo "=== Build complete ==="
echo "  Compiler:  $BUILD_DIR/casprix"
echo "  Runtime:   $BUILD_DIR/libcasprix_runtime.a"
