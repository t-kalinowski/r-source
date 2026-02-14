#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RSOURCE="$ROOT/r-source"
RSOURCE_REF="$ROOT/r-source-ref"
BUILD_MTL="$ROOT/builds/mtl"
BUILD_REF="$ROOT/builds/ref"

SOURCE="${TR_BOOTSTRAP_SOURCE:-https://github.com/wch/r-source.git}"
BRANCH="${TR_BOOTSTRAP_BRANCH:-trunk}"
PROFILE="${TR_BOOTSTRAP_PROFILE:-quick}" # quick | full
JOBS="${TR_BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
EXTRA_CPPFLAGS="${TR_BOOTSTRAP_CPPFLAGS:-}"
EXTRA_LDFLAGS="${TR_BOOTSTRAP_LDFLAGS:-}"

NO_BUILD=0
FORCE=0

log() { printf '[threaded-r bootstrap] %s\n' "$*"; }
die() { printf '[threaded-r bootstrap] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: harness/scripts/bootstrap.sh [options]

options:
  --source <path-or-git-url>   Upstream source (default: https://github.com/wch/r-source.git)
  --branch <name>              Upstream branch/tag (default: trunk)
  --profile <quick|full>       Build profile (default: quick)
  --jobs <n>                   Parallel make jobs (default: host CPU count)
  --no-build                   Configure only (skip make)
  --force                      Recreate build dirs and ref worktree
  -h, --help                   Show help

env overrides:
  TR_BOOTSTRAP_SOURCE, TR_BOOTSTRAP_BRANCH, TR_BOOTSTRAP_PROFILE, TR_BUILD_JOBS,
  TR_BOOTSTRAP_CPPFLAGS, TR_BOOTSTRAP_LDFLAGS
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source) SOURCE="$2"; shift 2 ;;
    --branch) BRANCH="$2"; shift 2 ;;
    --profile) PROFILE="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --no-build) NO_BUILD=1; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

case "$PROFILE" in
  quick) CONFIG_FLAGS=(--enable-R-shlib --without-x --disable-java --without-recommended-packages) ;;
  full) CONFIG_FLAGS=(--enable-R-shlib --without-x --disable-java) ;;
  *) die "invalid --profile '$PROFILE' (expected quick|full)" ;;
esac

if [[ -z "$EXTRA_CPPFLAGS" && -d /opt/homebrew/include ]]; then
  EXTRA_CPPFLAGS="-I/opt/homebrew/include"
fi
if [[ -z "$EXTRA_LDFLAGS" && -d /opt/homebrew/lib ]]; then
  EXTRA_LDFLAGS="-L/opt/homebrew/lib"
fi

mkdir -p "$ROOT/builds" "$ROOT/artifacts/bench/latest" "$ROOT/artifacts/checks/latest"

if [[ ! -d "$RSOURCE/.git" ]]; then
  log "initializing source repo at $RSOURCE"
  mkdir -p "$RSOURCE"
  git -C "$RSOURCE" init
fi

if git -C "$RSOURCE" remote get-url origin >/dev/null 2>&1; then
  git -C "$RSOURCE" remote set-url origin "$SOURCE"
else
  git -C "$RSOURCE" remote add origin "$SOURCE"
fi

log "syncing r-source to origin/$BRANCH"
git -C "$RSOURCE" fetch --tags --prune origin
git -C "$RSOURCE" checkout -B "$BRANCH" "origin/$BRANCH"

BASE_SHA="$(git -C "$RSOURCE" rev-parse HEAD)"
log "source sha: $BASE_SHA"

if [[ "$FORCE" == "1" && -d "$RSOURCE_REF" ]]; then
  log "removing existing reference worktree"
  git -C "$RSOURCE" worktree remove --force "$RSOURCE_REF"
fi

if [[ ! -d "$RSOURCE_REF/.git" ]]; then
  log "creating pinned reference worktree at $BASE_SHA"
  git -C "$RSOURCE" worktree add --detach "$RSOURCE_REF" "$BASE_SHA"
fi

if [[ "$FORCE" == "1" ]]; then
  rm -rf "$BUILD_MTL" "$BUILD_REF"
fi

mkdir -p "$BUILD_MTL" "$BUILD_REF"

configure_build() {
  local src="$1"
  local build="$2"
  local cppflags="${CPPFLAGS:-}"
  local ldflags="${LDFLAGS:-}"
  [[ -n "$EXTRA_CPPFLAGS" ]] && cppflags="${cppflags:+$cppflags }$EXTRA_CPPFLAGS"
  [[ -n "$EXTRA_LDFLAGS" ]] && ldflags="${ldflags:+$ldflags }$EXTRA_LDFLAGS"
  log "configuring $(basename "$build") from $(basename "$src")"
  (
    cd "$build"
    env CPPFLAGS="$cppflags" LDFLAGS="$ldflags" "$src/configure" "${CONFIG_FLAGS[@]}"
  )
}

configure_build "$RSOURCE" "$BUILD_MTL"
configure_build "$RSOURCE_REF" "$BUILD_REF"

if [[ "$NO_BUILD" == "1" ]]; then
  log "configure completed (--no-build)"
  exit 0
fi

log "building mtl (-j$JOBS)"
make -C "$BUILD_MTL" -j"$JOBS"
log "building ref (-j$JOBS)"
make -C "$BUILD_REF" -j"$JOBS"

log "bootstrap complete"
log "mtl R: $BUILD_MTL/bin/R"
log "ref R: $BUILD_REF/bin/R"
