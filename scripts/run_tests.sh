#!/usr/bin/env bash
set -euo pipefail

preset="${1:-dev}"
case "$preset" in
  dev|ci) ;;
  release)
    exec ctest --test-dir build/release --output-on-failure
    ;;
  *)
    echo "usage: $0 [dev|release|ci]" >&2
    exit 64
    ;;
esac

ctest --preset "$preset"
