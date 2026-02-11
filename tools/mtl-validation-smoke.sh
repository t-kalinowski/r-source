#!/bin/sh
#
# End-to-end MTL validation smoke:
# - package compatibility suite
# - serial performance regression guard
#
# Usage:
#   tools/mtl-validation-smoke.sh [build_dir] [system_lib] [threads] [standard_lib] [baseline_r] [serial_max_ratio]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
sys_lib="${2:-/Users/tomasz/Library/R/arm64/4.6/library}"
threads="${3:-4}"
standard_lib="${4:-${repo_root}/${build_dir}/library}"
baseline_r="${5:-/usr/local/bin/R-devel}"
serial_max_ratio="${6:-1.10}"

cd "${repo_root}"

"${repo_root}/tools/mtl-package-smoke.sh" "${build_dir}" "${sys_lib}" "${threads}" "${standard_lib}"
"${repo_root}/tools/mtl-perf-smoke.sh" "${build_dir}" "${baseline_r}" "${serial_max_ratio}"

echo
echo "full validation smoke ok"
