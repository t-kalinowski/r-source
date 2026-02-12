#!/bin/sh
#
# Run Shiny-style burst benchmark through background()/wait().
#
# Usage:
#   tools/mtl-shiny-background-smoke.sh [build_dir] [requests] [iters] [threads_csv] [min_bg_speedup] [work_iters] [timeout_sec]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
requests="${2:-96}"
iters="${3:-3}"
threads_csv="${4:-2,4,8}"
min_bg_speedup="${5:-1.5}"
work_iters="${6:-120000}"
timeout_sec="${7:-120}"

r_bin="${repo_root}/${build_dir}/bin/R"
if [ ! -x "${r_bin}" ]; then
  echo "R binary not found: ${r_bin}" >&2
  exit 2
fi

out_csv="${repo_root}/bench/results/mtl_shiny_background_smoke.csv"
out_png="${repo_root}/bench/figures/mtl_shiny_background_smoke.png"

if command -v /opt/homebrew/bin/gtimeout >/dev/null 2>&1; then
  /opt/homebrew/bin/gtimeout "${timeout_sec}" \
    "${r_bin}" --vanilla -q -f "${repo_root}/tools/mtl-shiny-background-smoke.R" --args \
    "${out_csv}" "${out_png}" "${requests}" "${iters}" "${threads_csv}" "${min_bg_speedup}" "${work_iters}"
else
  "${r_bin}" --vanilla -q -f "${repo_root}/tools/mtl-shiny-background-smoke.R" --args \
    "${out_csv}" "${out_png}" "${requests}" "${iters}" "${threads_csv}" "${min_bg_speedup}" "${work_iters}"
fi

echo
echo "shiny background smoke ok"
