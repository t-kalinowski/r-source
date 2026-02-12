#!/bin/sh
#
# End-to-end MTL validation smoke:
# - package compatibility suite
# - serial performance regression guard
# - framework-library binary namespace load sweep (macOS)
#
# Usage:
#   tools/mtl-validation-smoke.sh [build_dir] [system_lib] [threads] [standard_lib] [baseline_r] [serial_max_ratio] [framework_lib]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl}"
sys_lib="${2:-/Users/tomasz/Library/R/arm64/4.6/library}"
threads="${3:-4}"
standard_lib="${4:-${repo_root}/${build_dir}/library}"
baseline_r="${5:-/usr/local/bin/R-devel}"
serial_max_ratio="${6:-1.10}"
framework_lib="${7:-/Library/Frameworks/R.framework/Versions/4.6-arm64/Resources/library}"
soak_reps="${MTL_SOAK_REPS:-0}"

cd "${repo_root}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -e "cat('startup ok\\n')"

"${repo_root}/tools/mtl-core-runtime-smoke.sh" "${build_dir}" "3"
if [ "${soak_reps}" != "0" ]; then
  "${repo_root}/tools/mtl-soak-smoke.sh" "${build_dir}" "${soak_reps}"
fi

"${repo_root}/tools/mtl-package-smoke.sh" "${build_dir}" "${sys_lib}" "${threads}" "${standard_lib}"
"${repo_root}/tools/mtl-perf-smoke.sh" "${build_dir}" "${baseline_r}" "${serial_max_ratio}"
if [ "$(uname -s)" = "Darwin" ] && [ -d "${framework_lib}" ]; then
  "${repo_root}/tools/mtl-framework-library-smoke.sh" "${build_dir}" "${framework_lib}"
fi

echo
echo "full validation smoke ok"
