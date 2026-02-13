#!/bin/sh
#
# Core runtime smoke for threaded R internals:
# - repeated default startup with stats/utils load
# - mtlapply regression suite
# - background()/wait()/cancel() regression suite
# - notify read-fd wake regression suite
#
# Usage:
#   tools/mtl-core-runtime-smoke.sh [build_dir] [startup_reps]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
startup_reps="${2:-3}"

case "${startup_reps}" in
  ''|*[!0-9]*)
    echo "startup_reps must be a positive integer" >&2
    exit 2
    ;;
esac
if [ "${startup_reps}" -lt 1 ]; then
  echo "startup_reps must be >= 1" >&2
  exit 2
fi

r_bin="${repo_root}/${build_dir}/bin/R"
if [ ! -x "${r_bin}" ]; then
  echo "R binary not found: ${r_bin}" >&2
  exit 2
fi

i=1
while [ "${i}" -le "${startup_reps}" ]; do
  "${r_bin}" --vanilla -q -e "
    suppressPackageStartupMessages(library(stats))
    suppressPackageStartupMessages(library(utils))
    cat('startup rep ${i} ok\n')
  "
  i=$((i + 1))
done

"${r_bin}" --vanilla -q -f "${repo_root}/tests/mtlapply.R"
"${r_bin}" --vanilla -q -f "${repo_root}/tests/mtfuture.R"
"${r_bin}" --vanilla -q -f "${repo_root}/tools/mtl-notify-readfd-smoke.R" --args 10 0.3

echo
echo "core runtime smoke ok"
