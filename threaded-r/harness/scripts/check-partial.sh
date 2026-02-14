#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

tr_prepare_dirs
tr_log "partial checks: mode=$TR_MODE"
"$TR_ROOT/harness/scripts/check-smoke.sh"

if [[ -f "$TR_R_SOURCE/tests/mtlapply.R" ]]; then
  tr_log "running r-source/tests/mtlapply.R"
  tr_run_r_file "$TR_MTL_R_BIN" "$TR_R_SOURCE/tests/mtlapply.R"
fi

if [[ -f "$TR_R_SOURCE/tests/mtfuture.R" ]]; then
  tr_log "running r-source/tests/mtfuture.R"
  tr_run_r_file "$TR_MTL_R_BIN" "$TR_R_SOURCE/tests/mtfuture.R"
fi

if [[ -x "$TR_ROOT/harness/abi/run-abi-smoke.sh" ]]; then
  "$TR_ROOT/harness/abi/run-abi-smoke.sh"
fi

tr_stage_stamp "partial"
tr_log "partial checks passed"
