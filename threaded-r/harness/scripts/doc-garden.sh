#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAX_DAYS="${TR_DOC_STALE_DAYS:-30}"

printf '[threaded-r doc-garden] scanning docs for maintenance candidates\n'

now_epoch="$(date +%s)"

while IFS= read -r -d '' f; do
  m_epoch="$(stat -f %m "$f" 2>/dev/null || echo "$now_epoch")"
  age_days="$(( (now_epoch - m_epoch) / 86400 ))"
  if [[ "$age_days" -gt "$MAX_DAYS" ]]; then
    printf '[threaded-r doc-garden] stale-active-plan (%sd): %s\n' "$age_days" "${f#$ROOT/}"
  fi
done < <(find "$ROOT/docs/exec-plans/active" -type f -name '*.md' -print0)

if rg -n "TODO|FIXME|TBD" "$ROOT/docs" >/dev/null 2>&1; then
  printf '[threaded-r doc-garden] docs markers found:\n'
  rg -n "TODO|FIXME|TBD" "$ROOT/docs" || true
fi

printf '[threaded-r doc-garden] done\n'
