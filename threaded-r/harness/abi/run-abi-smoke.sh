#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/../scripts" && pwd)/common.sh"

tr_prepare_dirs
tr_log "ABI/package smoke: mode=$TR_MODE"

ABI_LIB="${TR_ABI_LIB:-$HOME/Library/R/arm64/4.6/library}"
PKGS_CSV="${TR_ABI_PACKAGES:-Matrix,digest,Rcpp}"

if [[ ! -d "$ABI_LIB" ]]; then
  tr_log "ABI lib path not found ($ABI_LIB), skipping"
  exit 0
fi

tr_run_r "$TR_MTL_R_BIN" -q -e "
  lib <- normalizePath('$ABI_LIB', winslash='/', mustWork=TRUE)
  .libPaths(c(lib, .libPaths()))
  pkgs <- strsplit('$PKGS_CSV', ',', fixed=TRUE)[[1L]]
  pkgs <- trimws(pkgs)
  pkgs <- pkgs[nzchar(pkgs)]
  for (p in pkgs) {
    suppressPackageStartupMessages(library(p, character.only=TRUE))
  }
  invisible(gc())
  cat('abi smoke ok\\n')
"

tr_stage_stamp "abi"
tr_log "ABI/package smoke passed"
