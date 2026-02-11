#!/bin/sh
#
# Run package compatibility smoke suites under the in-tree --enable-R-shlib build.
#
# Usage:
#   tools/mtl-package-smoke.sh [build_dir] [system_lib] [threads] [standard_lib]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
sys_lib="${2:-/Users/tomasz/Library/R/arm64/4.6/library}"
threads="${3:-4}"
std_lib="${4:-${repo_root}/${build_dir}/library}"

cd "${repo_root}"

"${repo_root}/tools/mtl-abi-macos.sh" "${build_dir}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-abi-smoke.R" \
  --args "${sys_lib}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-dplyr-smoke.R" \
  --args "${sys_lib}" "${threads}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-dropin-smoke.R" \
  --args "${threads}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-worker-native-smoke.R" \
  --args "${sys_lib}" "${threads}" "64"

"${repo_root}/tools/mtl-load-standard-library-smoke.sh" "${build_dir}" "${std_lib}"

echo
echo "package smoke ok"
