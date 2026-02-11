#!/bin/sh
#
# Make an in-tree --enable-R-shlib build ABI-compatible with macOS binary
# packages built against the R.framework.
#
# The key trick: set LC_ID_DYLIB of our in-tree libR.dylib (and libRblas/lapack)
# to the same absolute install-name used by the framework build:
#
#   /Library/Frameworks/R.framework/Versions/<ver>/Resources/lib/libR.dylib
#
# Once R has loaded our libR (via the normal build-tree paths), dyld considers
# that install-name already satisfied, so subsequently dlopening a package that
# depends on the framework libR will *not* load a second libR copy.
#
# This avoids crashes from having two libR instances in one process, and does
# not require rewriting installed packages.
#
# Usage:
#   tools/mtl-abi-macos.sh build-mtl
#
# Then you can run (example):
#   build-mtl/bin/R --vanilla -q -e \
#     '.libPaths(c("~/Library/R/arm64/4.6/library", .libPaths())); library(digest)'
#

set -eu

build_dir="${1:-build-mtl}"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "skip: mtl-abi-macos is only needed on macOS"
  exit 0
fi

if [ ! -d "${build_dir}" ]; then
  echo "error: build dir not found: ${build_dir}" >&2
  exit 1
fi

if [ ! -f "${build_dir}/lib/libR.dylib" ]; then
  echo "error: ${build_dir}/lib/libR.dylib not found. Build with --enable-R-shlib first." >&2
  exit 1
fi

current_id() {
  otool -D "$1" 2>/dev/null | sed -n '2p'
}

current_r_id="$(current_id "${build_dir}/lib/libR.dylib")"
if [ -n "${R_MTL_ABI_LIBROOT:-}" ]; then
  abi_lib_root="${R_MTL_ABI_LIBROOT}"
elif printf "%s" "${current_r_id}" | grep -q '^/Library/Frameworks/R\.framework/Versions/.*/Resources/lib/libR\.dylib$'; then
  abi_lib_root="$(dirname "${current_r_id}")"
else
  fw_ver="${R_MTL_FRAMEWORK_VER:-4.6-arm64}"
  abi_lib_root="/Library/Frameworks/R.framework/Versions/${fw_ver}/Resources/lib"
fi
id_r="${abi_lib_root}/libR.dylib"
id_blas="${abi_lib_root}/libRblas.dylib"
id_lapack="${abi_lib_root}/libRlapack.dylib"

if [ "$(current_id "${build_dir}/lib/libR.dylib")" = "${id_r}" ] &&
   [ "$(current_id "${build_dir}/lib/libRblas.dylib")" = "${id_blas}" ] &&
   [ "$(current_id "${build_dir}/lib/libRlapack.dylib")" = "${id_lapack}" ]; then
  echo "ok: install-names already mapped to framework ABI root"
  echo "  ${build_dir}/lib/libR.dylib -> ${id_r}"
  echo "  ${build_dir}/lib/libRblas.dylib -> ${id_blas}"
  echo "  ${build_dir}/lib/libRlapack.dylib -> ${id_lapack}"
  exit 0
fi

install_name_tool -id "${id_r}" "${build_dir}/lib/libR.dylib"
install_name_tool -id "${id_blas}" "${build_dir}/lib/libRblas.dylib"
install_name_tool -id "${id_lapack}" "${build_dir}/lib/libRlapack.dylib"

# Keep macOS code signing happy after install_name edits.
if [ -x "${build_dir}/bin/exec/R" ]; then
  codesign --force --sign - \
    "${build_dir}/lib/libR.dylib" \
    "${build_dir}/lib/libRblas.dylib" \
    "${build_dir}/lib/libRlapack.dylib" \
    "${build_dir}/bin/exec/R" >/dev/null 2>&1 || true
fi

echo "ok: set install-names for:"
echo "  ${build_dir}/lib/libR.dylib -> ${id_r}"
echo "  ${build_dir}/lib/libRblas.dylib -> ${id_blas}"
echo "  ${build_dir}/lib/libRlapack.dylib -> ${id_lapack}"
