#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

tr_prepare_dirs
tr_log "release validation: strict mode"
TR_MODE=strict "$SCRIPT_DIR/check-full.sh"

tr_log "release validation: drop-in mode"
TR_MODE=dropin "$SCRIPT_DIR/check-smoke.sh"

tr_stage_stamp "release"
tr_log "release validation passed"
