#!/bin/sh
#
# Threadpool speedup smoke:
# - uses fixed matrix-multiply workloads
# - checks for near-linear-ish speedup at configured thread count
#
# Usage:
#   tools/mtl-threadpool-perf-smoke.sh [build_dir] [threads] [reps] [min_eff] [out_csv]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
threads="${2:-8}"
reps="${3:-3}"
min_eff="${4:-0.50}"
out_csv="${5:-}"

mtl_r="${repo_root}/${build_dir}/bin/R"
if [ ! -x "${mtl_r}" ]; then
  echo "MTL R binary not found/executable: ${mtl_r}" >&2
  exit 1
fi

"${mtl_r}" --vanilla -q -f "${repo_root}/tools/mtl-threadpool-perf-smoke.R" --args \
  "${threads}" "${reps}" "${min_eff}" "${out_csv}"
