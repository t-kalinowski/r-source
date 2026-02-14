#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

stage="${1:-}"
if [[ -z "$stage" ]]; then
  cat <<'EOF'
usage: harness/scripts/run-stage.sh <stage>

stages:
  smoke
  partial
  full
  release
  bench-quick
  bench-full
EOF
  exit 1
fi

case "$stage" in
  smoke) "$SCRIPT_DIR/check-smoke.sh" ;;
  partial) "$SCRIPT_DIR/check-partial.sh" ;;
  full) "$SCRIPT_DIR/check-full.sh" ;;
  release) "$SCRIPT_DIR/validate-release.sh" ;;
  bench-quick) "$SCRIPT_DIR/bench-quick.sh" ;;
  bench-full) "$SCRIPT_DIR/bench-full.sh" ;;
  *) echo "unknown stage: $stage" >&2; exit 1 ;;
esac
