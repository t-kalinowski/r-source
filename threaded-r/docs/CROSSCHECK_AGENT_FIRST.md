# Cross-check: `threaded-r` vs Agent-first Harness Patterns

This maps the current scaffold against the design patterns in the February 11, 2026 harness-engineering post.

## Summary

- Strongly aligned on: concise `AGENTS.md`, progressive disclosure, staged validation harness, benchmark artifacts, and plan-oriented docs structure.
- Partially aligned on: mechanical docs freshness checks, debt management, and quality scoring.
- Missing/next: CI wiring and a recurring doc-gardening automation loop.

## Pattern-by-pattern check

1. Keep `AGENTS.md` short and map-like
- Status: aligned
- Evidence: `AGENTS.md` is concise and points to scoped docs.

2. Repository is the system of record
- Status: aligned
- Evidence: `docs/`, `plans/`, `benchmarks/`, and harness scripts are in-repo and versioned.

3. Progressive disclosure over monolithic instructions
- Status: aligned
- Evidence: scoped files in `agents/` and indexed docs.

4. Plans as first-class artifacts
- Status: aligned
- Evidence: `docs/exec-plans/active`, `docs/exec-plans/completed`, `PLAN_TEMPLATE.md`.

5. Mechanical enforcement of important rules
- Status: partially aligned
- Evidence: runtime/perf checks exist; docs checks added via `harness/scripts/check-docs.sh`.
- Remaining: expand docs checks (e.g., link integrity, stale-plan policy).

6. Continuous cleanup / entropy control
- Status: partially aligned
- Evidence: `harness/scripts/doc-garden.sh` and debt tracker introduced.
- Remaining: scheduled automation loop and CI integration.

7. Quality tracking and known-gap visibility
- Status: aligned
- Evidence: `docs/QUALITY_SCORE.md`, `docs/exec-plans/tech-debt-tracker.md`.

8. Tooling-first iteration loops
- Status: aligned
- Evidence: Make targets + staged harness (`smoke`, `partial`, `full`, `release`, benchmark targets).

## Concrete updates made in this pass

- Added durable design-doc index and principles:
  - `docs/design-docs/index.md`
  - `docs/design-docs/core-beliefs.md`
- Added operational debt and quality tracking:
  - `docs/exec-plans/tech-debt-tracker.md`
  - `docs/QUALITY_SCORE.md`
- Added mechanical docs guardrail:
  - `harness/scripts/check-docs.sh`
  - `make docs-check`
- Added doc-gardening helper:
  - `harness/scripts/doc-garden.sh`
  - `make doc-garden`
- Wired docs-check into smoke stage:
  - `harness/scripts/check-smoke.sh`

## Next upgrades

1. Add CI job(s) to run `make docs-check`, `make smoke`, and benchmark threshold checks.
2. Add link-check/staleness checks to `check-docs.sh`.
3. Promote recurring doc-gardening to automated cadence with artifact/report output.
