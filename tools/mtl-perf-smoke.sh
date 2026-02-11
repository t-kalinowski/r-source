#!/bin/sh
#
# Performance regression smoke:
# - runs benchmark artifacts for baseline R and MTL R
# - checks serial lapply parity against a threshold
#
# Usage:
#   tools/mtl-perf-smoke.sh [build_dir] [baseline_r] [serial_max_ratio]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl}"
baseline_r="${2:-/usr/local/bin/R-devel}"
serial_max_ratio="${3:-1.10}"
mtl_r="${repo_root}/${build_dir}/bin/R"

if [ ! -x "${baseline_r}" ]; then
  echo "baseline R binary not found/executable: ${baseline_r}" >&2
  exit 1
fi
if [ ! -x "${mtl_r}" ]; then
  echo "MTL R binary not found/executable: ${mtl_r}" >&2
  exit 1
fi

tmp_dir="${repo_root}/.tmp-mtl-perf"
rm -rf "${tmp_dir}"
mkdir -p "${tmp_dir}"

base_out="${tmp_dir}/baseline.rds"
mtl_out="${tmp_dir}/mtl.rds"

cd "${repo_root}"

echo "running baseline benchmark: ${baseline_r}"
README_ITERS="${README_ITERS:-5}" README_THREADS="${README_THREADS:-1,2,4,8}" \
  "${baseline_r}" --vanilla -q -f bench/readme_bench_run.R --args "${base_out}"

echo "running mtl benchmark: ${mtl_r}"
README_ITERS="${README_ITERS:-5}" README_THREADS="${README_THREADS:-1,2,4,8}" \
  "${mtl_r}" --vanilla -q -f bench/readme_bench_run.R --args "${mtl_out}"

echo "checking regression threshold: ${serial_max_ratio}"
"${mtl_r}" --vanilla -q -f tools/mtl-perf-regression-check.R --args \
  "${mtl_out}" "${base_out}" "${serial_max_ratio}"

echo
echo "performance smoke ok"
