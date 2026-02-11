#!/bin/sh
#
# Run the dplyr ABI smoke test under the in-tree --enable-R-shlib build.
#
# This ensures:
# - the build's libR.dylib has the same install-name as the R.framework libR,
#   so binary packages built against the framework can be dlopened safely.
# - dplyr can be loaded from the system binary library and basic examples run.
#
# Usage:
#   tools/mtl-dplyr-smoke.sh [build_dir] [system_lib] [threads]
#

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
sys_lib="${2:-/Users/tomasz/Library/R/arm64/4.6/library}"
threads="${3:-4}"

cd "${repo_root}"

"${repo_root}/tools/mtl-abi-macos.sh" "${build_dir}"

"${repo_root}/${build_dir}/bin/R" --vanilla -q -f "${repo_root}/tools/mtl-dplyr-smoke.R" \
  --args "${sys_lib}" "${threads}"

