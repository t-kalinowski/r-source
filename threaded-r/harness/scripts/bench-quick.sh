#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

tr_prepare_dirs
tr_log "quick benchmark refresh"
tr_check_libpaths "$TR_MTL_R_BIN"

RESULT_CSV="$TR_ARTIFACTS/bench/latest/results-quick.csv"
REPORT_HTML="$TR_ARTIFACTS/bench/latest/dashboard-quick.html"

TR_BENCH_ITERS="${TR_BENCH_ITERS:-3}" \
TR_BENCH_THREADS="${TR_BENCH_THREADS:-1,2,4}" \
TR_BENCH_SHA="$(tr_git_sha)" \
tr_run_r_file "$TR_MTL_R_BIN" "$TR_ROOT/benchmarks/scripts/run_benchmarks.R" --args "$RESULT_CSV"

if [[ -f "$TR_ROOT/benchmarks/baselines/serial_baseline.csv" ]]; then
  tr_run_r_file "$TR_MTL_R_BIN" "$TR_ROOT/benchmarks/scripts/render_dashboard.R" --args \
    "$RESULT_CSV" "$TR_ROOT/benchmarks/baselines/serial_baseline.csv" "$REPORT_HTML"
fi

cp "$RESULT_CSV" "$TR_ARTIFACTS/bench/latest/summary.csv"
cp "$RESULT_CSV" "$TR_ARTIFACTS/bench/history/results-quick-$(date -u +%Y%m%dT%H%M%SZ)-$(tr_git_sha).csv"
tr_log "quick benchmark artifacts updated"
