#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

mkdir -p \
  "$ROOT/r-source" \
  "$ROOT/builds/mtl" \
  "$ROOT/builds/ref" \
  "$ROOT/builds/mtl-clang" \
  "$ROOT/artifacts/bench/latest" \
  "$ROOT/artifacts/bench/history" \
  "$ROOT/artifacts/checks/latest" \
  "$ROOT/artifacts/checks/history" \
  "$ROOT/site-library-strict"

echo "layout initialized under: $ROOT"
