#!/bin/sh
#
# Smoke test RStudio rsession startup against an in-tree MTL R build.
# This does not launch the GUI app.
#
# Usage:
#   tools/mtl-rsession-smoke.sh [build_dir] [rstudio_app]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
rstudio_app="${2:-/Applications/RStudio.app}"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "skip: rsession smoke is macOS-only"
  exit 0
fi

r_home="${repo_root}/${build_dir}"
r_lib="${r_home}/lib/libR.dylib"
if [ ! -f "${r_lib}" ]; then
  echo "error: missing libR at ${r_lib}" >&2
  exit 1
fi

rsession_dir="${rstudio_app}/Contents/Resources/app/bin"
if [ ! -d "${rsession_dir}" ]; then
  echo "skip: RStudio app bin dir not found: ${rsession_dir}"
  exit 0
fi

arch_info=$(file "${r_lib}" 2>/dev/null || true)
rsession_bin="${rsession_dir}/rsession"
if printf "%s" "${arch_info}" | grep -q "arm64" && [ -x "${rsession_dir}/rsession-arm64" ]; then
  rsession_bin="${rsession_dir}/rsession-arm64"
fi
if [ ! -x "${rsession_bin}" ]; then
  echo "skip: rsession binary not found: ${rsession_bin}"
  exit 0
fi

tmp_root=$(mktemp -d /tmp/mtl-rsession-smoke.XXXXXX)
trap 'rm -rf "${tmp_root}"' EXIT INT TERM

mkdir -p "${tmp_root}/home" "${tmp_root}/data" "${tmp_root}/config" "${tmp_root}/cache"

# Mirror desktop launcher behavior for dyld fallback paths.
fallback_lib="${tmp_root}/fallback-lib"
mkdir -p "${fallback_lib}"
dyld_fallback="${r_home}/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}:${fallback_lib}"
dyld_library="${r_home}/lib:${DYLD_LIBRARY_PATH:-}"

log_file="${tmp_root}/rsession.log"

set +e
HOME="${tmp_root}/home" \
XDG_DATA_HOME="${tmp_root}/data" \
XDG_CONFIG_HOME="${tmp_root}/config" \
XDG_CACHE_HOME="${tmp_root}/cache" \
R_HOME="${r_home}" \
R_DOC_DIR="${r_home}/doc" \
DYLD_INSERT_LIBRARIES="${r_lib}" \
DYLD_FALLBACK_LIBRARY_PATH="${dyld_fallback}" \
DYLD_LIBRARY_PATH="${dyld_library}" \
RSTUDIO_WHICH_R="${r_home}/bin/R" \
"${rsession_bin}" \
  --log-stderr 1 \
  --verify-installation 1 >"${log_file}" 2>&1
rc=$?
set -e

if [ "${rc}" -eq 0 ]; then
  echo "rsession smoke ok (${rsession_bin})"
  exit 0
fi

# In restricted sandboxes, local/TCP listeners can fail with EPERM.
if rg -q "Operation not permitted.*init(LocalStreamAcceptor|TcpIpAcceptor)|system error 1 \\(Operation not permitted\\)" "${log_file}"; then
  echo "skip: rsession listener setup blocked by sandbox permissions"
  exit 0
fi

echo "rsession smoke failed (${rsession_bin}), exit=${rc}" >&2
tail -n 200 "${log_file}" >&2 || true
exit "${rc}"
