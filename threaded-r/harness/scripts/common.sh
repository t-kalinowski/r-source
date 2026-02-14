#!/usr/bin/env bash
set -euo pipefail

TR_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TR_R_SOURCE="${TR_R_SOURCE:-$TR_ROOT/r-source}"
TR_BUILDS="${TR_BUILDS:-$TR_ROOT/builds}"
TR_ARTIFACTS="${TR_ARTIFACTS:-$TR_ROOT/artifacts}"

TR_MTL_BUILD="${TR_MTL_BUILD:-$TR_BUILDS/mtl}"
TR_REF_BUILD="${TR_REF_BUILD:-$TR_BUILDS/ref}"
TR_MTL_CLANG_BUILD="${TR_MTL_CLANG_BUILD:-$TR_BUILDS/mtl-clang}"

TR_MTL_R_BIN="${TR_MTL_R_BIN:-$TR_MTL_BUILD/bin/R}"
TR_REF_R_BIN="${TR_REF_R_BIN:-$TR_REF_BUILD/bin/R}"
TR_SYSTEM_R_BIN="${TR_SYSTEM_R_BIN:-/usr/local/bin/R-devel}"

TR_MODE="${TR_MODE:-strict}" # strict | dropin
TR_STRICT_USER_LIB="${TR_STRICT_USER_LIB:-$TR_ROOT/site-library-strict}"
TR_ALLOW_FRAMEWORK_SITE="${TR_ALLOW_FRAMEWORK_SITE:-1}"

tr_log() {
  printf '[threaded-r] %s\n' "$*"
}

tr_die() {
  printf '[threaded-r] ERROR: %s\n' "$*" >&2
  exit 1
}

tr_require_file() {
  local p="$1"
  [[ -f "$p" ]] || tr_die "required file not found: $p"
}

tr_require_exe() {
  local p="$1"
  [[ -x "$p" ]] || tr_die "required executable not found: $p"
}

tr_git_sha() {
  if command -v git >/dev/null 2>&1; then
    git -C "$TR_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown"
  else
    echo "unknown"
  fi
}

tr_timestamp_utc() {
  date -u +"%Y-%m-%dT%H:%M:%SZ"
}

tr_prepare_dirs() {
  mkdir -p \
    "$TR_ARTIFACTS/bench/latest" \
    "$TR_ARTIFACTS/bench/history" \
    "$TR_ARTIFACTS/checks/latest" \
    "$TR_ARTIFACTS/checks/history" \
    "$TR_STRICT_USER_LIB"
}

tr_env_prefix() {
  local -a envs=(
    "R_ENVIRON_USER="
    "R_PROFILE_USER="
    "R_PROFILE="
    "R_HISTFILE=/dev/null"
    "TR_MODE=$TR_MODE"
    "TR_ALLOWED_PREFIX=$TR_ROOT"
    "TR_STRICT_USER_LIB=$TR_STRICT_USER_LIB"
    "TR_ALLOW_FRAMEWORK_SITE=$TR_ALLOW_FRAMEWORK_SITE"
  )
  if [[ "$TR_MODE" == "strict" ]]; then
    envs+=(
      "R_LIBS="
      "R_LIBS_SITE="
      "R_LIBS_USER=$TR_STRICT_USER_LIB"
    )
  fi
  printf '%s\0' "${envs[@]}"
}

tr_run_r() {
  local r_bin="$1"
  shift
  tr_require_exe "$r_bin"

  local -a envs=()
  while IFS= read -r -d '' item; do
    envs+=("$item")
  done < <(tr_env_prefix)

  env "${envs[@]}" "$r_bin" --vanilla "$@"
}

tr_run_r_file() {
  local r_bin="$1"
  local file="$2"
  shift 2
  tr_require_file "$file"
  tr_run_r "$r_bin" -q -f "$file" "$@"
}

tr_check_libpaths() {
  local r_bin="$1"
  tr_run_r_file "$r_bin" "$TR_ROOT/harness/checks/check-libpaths.R"
}

tr_stage_stamp() {
  local stage="$1"
  local out="$TR_ARTIFACTS/checks/latest/${stage}.status"
  cat > "$out" <<EOF
stage=${stage}
mode=${TR_MODE}
sha=$(tr_git_sha)
timestamp_utc=$(tr_timestamp_utc)
host=$(hostname)
EOF
  cp "$out" "$TR_ARTIFACTS/checks/history/${stage}-$(date -u +%Y%m%dT%H%M%SZ)-$(tr_git_sha).status"
}
