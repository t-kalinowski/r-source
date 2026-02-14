#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

tr_prepare_dirs
tr_log "full checks: mode=$TR_MODE"
"$TR_ROOT/harness/scripts/check-partial.sh"
"$TR_ROOT/harness/scripts/bench-full.sh"

if [[ "${TR_RUN_CHECK_ALL:-0}" == "1" && -f "$TR_MTL_BUILD/Makefile" ]]; then
  tr_log "running make -C $TR_MTL_BUILD check-all"
  make -C "$TR_MTL_BUILD" check-all
fi

tr_stage_stamp "full"
tr_log "full checks passed"
