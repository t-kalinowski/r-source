#!/bin/sh
#
# Minimal smoke wrapper for notify read-fd wakeups.
#
# Usage:
#   tools/mtl-notify-readfd-smoke.sh [build_dir] [rounds] [deadline_sec]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
rounds="${2:-20}"
deadline="${3:-0.5}"

r_bin="${repo_root}/${build_dir}/bin/R"
if [ ! -x "${r_bin}" ]; then
  echo "R binary not found: ${r_bin}" >&2
  exit 2
fi

"${r_bin}" --vanilla -q -f "${repo_root}/tools/mtl-notify-readfd-smoke.R" --args "${rounds}" "${deadline}"
