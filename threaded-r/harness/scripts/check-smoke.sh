#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

tr_prepare_dirs
tr_log "smoke checks: mode=$TR_MODE"
"$TR_ROOT/harness/scripts/check-docs.sh"
tr_check_libpaths "$TR_MTL_R_BIN"
tr_run_r_file "$TR_MTL_R_BIN" "$TR_ROOT/harness/checks/smoke-mtlapply.R"
tr_run_r_file "$TR_MTL_R_BIN" "$TR_ROOT/harness/checks/smoke-futures.R"
tr_stage_stamp "smoke"
tr_log "smoke checks passed"
