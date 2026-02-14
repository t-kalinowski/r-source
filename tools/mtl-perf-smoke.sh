#!/bin/sh
#
# Performance regression smoke:
# - runs benchmark artifacts for baseline R and MTL R
# - checks serial lapply parity against a threshold
# - checks minimal serial-kernel parity against a threshold
# - checks threadpool speedup on fixed matmul workloads
#
# Usage:
#   tools/mtl-perf-smoke.sh [build_dir] [baseline_r] [serial_max_ratio] [speedup_threads] [speedup_reps] [speedup_min_eff]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl}"
baseline_r="${2:-/usr/local/bin/R-devel}"
serial_max_ratio="${3:-1.10}"
speedup_threads="${4:-8}"
speedup_reps="${5:-3}"
speedup_min_eff="${6:-0.50}"
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
base_min_out="${tmp_dir}/baseline_serial_minimal.rds"
mtl_min_out="${tmp_dir}/mtl_serial_minimal.rds"

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

echo "running baseline minimal serial kernels: ${baseline_r}"
SERIAL_MIN_ITERS="${SERIAL_MIN_ITERS:-5}" \
  "${baseline_r}" --vanilla -q -f bench/serial_minimal_bench.R --args "${base_min_out}" baseline

echo "running mtl minimal serial kernels: ${mtl_r}"
SERIAL_MIN_ITERS="${SERIAL_MIN_ITERS:-5}" \
  "${mtl_r}" --vanilla -q -f bench/serial_minimal_bench.R --args "${mtl_min_out}" mtl

echo "checking minimal serial-kernel threshold: ${serial_max_ratio}"
"${mtl_r}" --vanilla -q -f tools/mtl-serial-minimal-check.R --args \
  "${mtl_min_out}" "${base_min_out}" "${serial_max_ratio}"

echo "checking threadpool speedup smoke: threads=${speedup_threads} reps=${speedup_reps} min_eff=${speedup_min_eff}"
"${repo_root}/tools/mtl-threadpool-perf-smoke.sh" "${build_dir}" "${speedup_threads}" "${speedup_reps}" "${speedup_min_eff}"

if [ "${MTL_SHINY_BG_SMOKE:-0}" = "1" ]; then
  shiny_requests="${MTL_SHINY_BG_REQUESTS:-96}"
  shiny_iters="${MTL_SHINY_BG_ITERS:-3}"
  shiny_threads="${MTL_SHINY_BG_THREADS:-2,4,8}"
  shiny_min_speedup="${MTL_SHINY_BG_MIN_SPEEDUP:-1.5}"
  shiny_work_iters="${MTL_SHINY_BG_WORK_ITERS:-120000}"
  echo "checking shiny background smoke: requests=${shiny_requests} iters=${shiny_iters} threads=${shiny_threads} min_speedup=${shiny_min_speedup} work_iters=${shiny_work_iters}"
  "${repo_root}/tools/mtl-shiny-background-smoke.sh" "${build_dir}" \
    "${shiny_requests}" "${shiny_iters}" "${shiny_threads}" "${shiny_min_speedup}" "${shiny_work_iters}"
fi

echo
echo "performance smoke ok"
