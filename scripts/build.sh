#!/usr/bin/env bash
set -euo pipefail

preset="${1:-dev}"
case "$preset" in
  dev|release|ci) ;;
  *)
    echo "usage: $0 [dev|release|ci]" >&2
    exit 64
    ;;
esac

cmake --preset "$preset"
cmake --build --preset "$preset" --parallel
