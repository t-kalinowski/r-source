#!/bin/sh
#
# Configure a canonical MTL build directory.
#
# Usage:
#   tools/mtl-configure.sh [build_dir] [-- <extra configure args>...]
#
# Defaults:
# - build directory: build-mtl
# - enables shared libR (--enable-R-shlib)
# - ABI install-name mapping is applied after build via:
#   tools/mtl-abi-macos.sh <build_dir>
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl}"
if [ $# -ge 1 ]; then
  shift
fi

if [ "${1:-}" = "--" ]; then
  shift
fi

mkdir -p "${repo_root}/${build_dir}"
cd "${repo_root}/${build_dir}"

exec ../configure --enable-R-shlib "$@"
