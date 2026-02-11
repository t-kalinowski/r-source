#!/bin/sh
#
# Validate loading package namespaces from a framework/system R library tree
# (pre-built binaries), e.g.:
#   /Library/Frameworks/R.framework/Versions/4.6-arm64/Resources/library
#
# Usage:
#   tools/mtl-framework-library-smoke.sh [build_dir] [framework_lib]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
framework_lib="${2:-/Library/Frameworks/R.framework/Versions/4.6-arm64/Resources/library}"
mtl_r="${repo_root}/${build_dir}/bin/R"

if [ ! -x "${mtl_r}" ]; then
  echo "MTL R binary not found/executable: ${mtl_r}" >&2
  exit 1
fi
if [ ! -d "${framework_lib}" ]; then
  echo "framework library path not found: ${framework_lib}" >&2
  exit 1
fi

cd "${repo_root}"

echo "framework package library: ${framework_lib}"
"${mtl_r}" --vanilla -q -f "${repo_root}/tools/mtl-load-library-smoke.R" --args "${framework_lib}"

echo
echo "framework-library load smoke ok"
