#!/bin/sh
#
# Run the package compatibility smoke suite under the in-tree --enable-R-shlib build.
#
# Usage:
#   tools/mtl-package-smoke.sh [build_dir] [system_lib] [threads]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
sys_lib="${2:-/Users/tomasz/Library/R/arm64/4.6/library}"
threads="${3:-4}"

cd "${repo_root}"

"${repo_root}/tools/mtl-abi-macos.sh" "${build_dir}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-abi-smoke.R" \
  --args "${sys_lib}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-dplyr-smoke.R" \
  --args "${sys_lib}" "${threads}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-worker-native-smoke.R" \
  --args "${sys_lib}" "${threads}" "64"

echo
echo "package smoke ok"
