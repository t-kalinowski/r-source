#!/bin/sh
#
# Launch RStudio against an in-tree MTL R build on macOS.
#
# Why this script:
# - `RSTUDIO_WHICH_R=... open <project>.Rproj` is unreliable on macOS because
#   LaunchServices does not consistently propagate shell env vars.
# - After rebuilds, our local libR install-names may need re-fixup for binary
#   package compatibility.
#
# Usage:
#   tools/launch-rstudio-mtl.sh [build_dir] [project]
#
# Examples:
#   tools/launch-rstudio-mtl.sh
#   tools/launch-rstudio-mtl.sh build-mtl-shlib ~/github/t-kalinowski/quickr/quickr.Rproj
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
project="${2:-}"

rbin="${repo_root}/${build_dir}/bin/R"
rstudio_bin="/Applications/RStudio.app/Contents/MacOS/RStudio"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "error: this launcher is for macOS" >&2
  exit 1
fi

if [ ! -x "${rbin}" ]; then
  echo "error: R binary not found: ${rbin}" >&2
  exit 1
fi

if [ ! -x "${rstudio_bin}" ]; then
  echo "error: RStudio binary not found: ${rstudio_bin}" >&2
  exit 1
fi

# Keep ABI install-name mapping in sync for this build tree.
bash "${repo_root}/tools/mtl-abi-macos.sh" "${build_dir}" >/dev/null

echo "Launching RStudio with:"
echo "  RSTUDIO_WHICH_R=${rbin}"

if [ -n "${project}" ]; then
  exec env RSTUDIO_WHICH_R="${rbin}" "${rstudio_bin}" "${project}"
else
  exec env RSTUDIO_WHICH_R="${rbin}" "${rstudio_bin}"
fi
