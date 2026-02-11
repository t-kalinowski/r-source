#!/bin/sh
#
# Validate that the MTL build can load all packages from a standard built-library set.
#
# Usage:
#   tools/mtl-load-standard-library-smoke.sh [build_dir] [library_path]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl}"
mtl_r="${repo_root}/${build_dir}/bin/R"
lib_path="${2:-${repo_root}/${build_dir}/library}"

if [ ! -x "${mtl_r}" ]; then
  echo "MTL R binary not found/executable: ${mtl_r}" >&2
  exit 1
fi
if [ ! -d "${lib_path}" ]; then
  echo "library path not found: ${lib_path}" >&2
  exit 1
fi

cd "${repo_root}"

echo "built package library: ${lib_path}"
"${mtl_r}" --vanilla -q -f "${repo_root}/tools/mtl-load-library-smoke.R" --args "${lib_path}"

echo
echo "standard-library load smoke ok"
