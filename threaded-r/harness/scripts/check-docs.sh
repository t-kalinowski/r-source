#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

die() {
  printf '[threaded-r docs-check] ERROR: %s\n' "$*" >&2
  exit 1
}

required_files=(
  "$ROOT/AGENTS.md"
  "$ROOT/docs/README.md"
  "$ROOT/docs/KNOWLEDGE_BASE_LAYOUT.md"
  "$ROOT/docs/THREADING_PRODUCT_DIRECTION.md"
  "$ROOT/docs/CURRENT_IMPLEMENTATION_DESIGN.md"
  "$ROOT/docs/INVARIANTS_CHECKLIST.md"
  "$ROOT/docs/AGENT_FIRST_PATTERNS.md"
  "$ROOT/docs/QUALITY_SCORE.md"
  "$ROOT/docs/design-docs/index.md"
  "$ROOT/docs/design-docs/core-beliefs.md"
  "$ROOT/docs/exec-plans/PLAN_TEMPLATE.md"
  "$ROOT/docs/exec-plans/tech-debt-tracker.md"
)

for f in "${required_files[@]}"; do
  [[ -f "$f" ]] || die "missing required file: ${f#$ROOT/}"
done

[[ -d "$ROOT/docs/exec-plans/active" ]] || die "missing docs/exec-plans/active"
[[ -d "$ROOT/docs/exec-plans/completed" ]] || die "missing docs/exec-plans/completed"

agents_lines="$(wc -l < "$ROOT/AGENTS.md" | tr -d ' ')"
if [[ "$agents_lines" -gt 180 ]]; then
  die "AGENTS.md too long ($agents_lines lines); keep it short/map-like"
fi

while IFS= read -r -d '' plan; do
  grep -q '^## Status' "$plan" || die "plan missing '## Status': ${plan#$ROOT/}"
  grep -q '^## Decision log' "$plan" || die "plan missing '## Decision log': ${plan#$ROOT/}"
done < <(find "$ROOT/docs/exec-plans/active" "$ROOT/docs/exec-plans/completed" -type f -name '*.md' ! -name '.gitkeep' -print0)

printf '[threaded-r docs-check] ok\n'
