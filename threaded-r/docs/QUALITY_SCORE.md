# Quality Score

This scorecard tracks how close the repository is to the intended agent-first operating model.

Scoring scale: 0 (absent) to 5 (strongly established).

## Current snapshot

- Knowledge layout clarity: 4/5
Rationale: concise `AGENTS.md`, scoped docs, and explicit structure exist.

- Mechanical guardrails: 3/5
Rationale: runtime smoke/perf checks exist; docs checks are newly added and still basic.

- Plan discipline: 3/5
Rationale: plan directories and templates exist; active/completed flow needs routine use.

- Benchmark legibility: 4/5
Rationale: CSV-driven benchmarks + dashboard + threshold checks are in place.

- Package compatibility validation: 3/5
Rationale: ABI smoke exists; broader package matrix remains to be expanded.

- Cleanup/entropy management: 2/5
Rationale: principles exist, but recurring doc-gardening workflow is still immature.

## Improvement priorities

1. Raise mechanical docs checks to include link validation and stale-plan checks.
2. Add CI integration for staged harness commands with artifact upload.
3. Run regular debt cleanup passes and move resolved items to closed.
