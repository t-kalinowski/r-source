#!/bin/sh
#
# Regenerate committed benchmark artifacts and markdown summaries.
#
# Usage:
#   tools/mtl-bench-refresh.sh [build_dir] [system_r] [rdevel_r]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
system_r="${2:-R}"
rdevel_r="${3:-/usr/local/bin/R-devel}"
mtl_r="${repo_root}/${build_dir}/bin/R"

if ! command -v "${system_r}" >/dev/null 2>&1; then
  echo "system R binary not found on PATH: ${system_r}" >&2
  exit 1
fi
if [ ! -x "${rdevel_r}" ]; then
  echo "R-devel binary not found/executable: ${rdevel_r}" >&2
  exit 1
fi
if [ ! -x "${mtl_r}" ]; then
  echo "MTL R binary not found/executable: ${mtl_r}" >&2
  exit 1
fi

cd "${repo_root}"

echo "==> system benchmark artifact"
"${system_r}" --vanilla -q -f bench/readme_bench_run.R --args bench/results/system_latest.rds

echo "==> r-devel benchmark artifact"
"${rdevel_r}" --vanilla -q -f bench/readme_bench_run.R --args bench/results/rdevel_latest.rds

echo "==> mtl benchmark artifact"
"${mtl_r}" --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl_latest.rds

echo "==> r-devel minimal serial kernels"
SERIAL_MIN_ITERS="${SERIAL_MIN_ITERS:-5}" \
  "${rdevel_r}" --vanilla -q -f bench/serial_minimal_bench.R --args \
  bench/results/serial_minimal_rdevel_latest.rds rdevel

echo "==> mtl minimal serial kernels"
SERIAL_MIN_ITERS="${SERIAL_MIN_ITERS:-5}" \
  "${mtl_r}" --vanilla -q -f bench/serial_minimal_bench.R --args \
  bench/results/serial_minimal_mtl_latest.rds mtl

echo "==> threadpool benchmark artifact"
"${mtl_r}" --vanilla -q -f tools/mtl-threadpool-perf-smoke.R --args \
  8 3 0.45 bench/results/threadpool_perf_checkpoint.csv

echo "==> background/wait benchmark artifact"
"${mtl_r}" --vanilla -q -f tools/mtl-shiny-background-smoke.R --args \
  bench/results/mtl_shiny_background_smoke_checkpoint.csv \
  bench/figures/mtl_shiny_background_smoke_checkpoint.png \
  96 3 2,4,8 1.5 120000

echo "==> benchmark markdown summary"
"${mtl_r}" --vanilla -q -f bench/render_bench_report.R --args bench/LATEST.md

echo "==> README render"
"${mtl_r}" --vanilla -q -e 'rmarkdown::render("README.Rmd", output_format = "github_document")'

echo
echo "benchmark artifacts refreshed"
echo "review with: git diff -- bench/LATEST.md README.md bench/results/"
