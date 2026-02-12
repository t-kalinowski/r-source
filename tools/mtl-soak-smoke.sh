#!/bin/sh
#
# Flakiness soak for threaded runtime regressions.
# Runs core regression scripts in fresh R processes repeatedly.
#
# Usage:
#   tools/mtl-soak-smoke.sh [build_dir] [reps]
#
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="${1:-build-mtl-shlib}"
reps="${2:-25}"

case "${reps}" in
  ''|*[!0-9]*)
    echo "reps must be a positive integer" >&2
    exit 2
    ;;
esac
if [ "${reps}" -lt 1 ]; then
  echo "reps must be >= 1" >&2
  exit 2
fi

r_bin="${repo_root}/${build_dir}/bin/R"
if [ ! -x "${r_bin}" ]; then
  echo "R binary not found: ${r_bin}" >&2
  exit 2
fi

i=1
while [ "${i}" -le "${reps}" ]; do
  log_a=$(mktemp /tmp/mtl-soak-a.XXXXXX)
  log_b=$(mktemp /tmp/mtl-soak-b.XXXXXX)
  trap 'rm -f "${log_a}" "${log_b}"' EXIT INT TERM

  if ! "${r_bin}" --vanilla -q -f "${repo_root}/tests/mtlapply.R" >"${log_a}" 2>&1; then
    echo "soak failure at rep ${i}: tests/mtlapply.R" >&2
    tail -n 120 "${log_a}" >&2 || true
    rm -f "${log_a}" "${log_b}"
    exit 1
  fi
  if ! "${r_bin}" --vanilla -q -f "${repo_root}/tests/mtfuture.R" >"${log_b}" 2>&1; then
    echo "soak failure at rep ${i}: tests/mtfuture.R" >&2
    tail -n 120 "${log_b}" >&2 || true
    rm -f "${log_a}" "${log_b}"
    exit 1
  fi

  rm -f "${log_a}" "${log_b}"

  if [ $((i % 5)) -eq 0 ] || [ "${i}" -eq "${reps}" ]; then
    echo "soak progress: ${i}/${reps}"
  fi

  i=$((i + 1))
done

echo "soak smoke ok (${reps} reps)"
