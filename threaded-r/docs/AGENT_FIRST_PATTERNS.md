# Agent-First Patterns for Threaded R

This document distills practical patterns from high-throughput, agent-driven engineering and adapts them to the goals of this project.

## 1) Humans steer, agents execute

Apply this as a hard operating model:

- humans define outcomes, constraints, and acceptance criteria;
- agents perform implementation, review loops, and most mechanical iteration;
- failures are treated as missing capability in the harness, not as one-off mistakes.

Project implication:

- when a task repeatedly fails, add a reusable harness capability (script, doc, lint, smoke test) instead of relying on prompt retries.

## 2) Keep `AGENTS.md` short (table of contents only)

Do not turn `AGENTS.md` into a giant manual.

- keep it stable and small;
- point to scoped docs and plans;
- optimize for progressive disclosure.

Why:

- context is scarce;
- large monolith docs crowd out task-specific reasoning;
- stale global docs become noise quickly.

## 3) Repository is the system of record

Anything not in-repo is effectively invisible to agents.

For this project, keep these in versioned markdown:

- invariants and constraints,
- API decisions,
- active execution plans,
- benchmark interpretation,
- known debt and open risks.

Avoid relying on off-repo chat context for critical decisions.

## 4) Plans are first-class artifacts

Capture non-trivial work in execution plans under `docs/exec-plans/`.

Minimum plan sections:

1. goal and non-goals,
2. invariants touched,
3. file-level scope,
4. validation ladder,
5. rollback strategy,
6. status log.

Move plans from `active/` to `completed/` when done.

## 5) Enforce invariants mechanically

Prefer machine-checked constraints over policy prose.

For this codebase, high-value enforcement targets:

- serial parity thresholds,
- benchmark artifact freshness,
- smoke-suite pass requirements,
- architectural boundary checks in runtime files,
- docs cross-link and freshness checks.

Pattern:

- if a rule matters repeatedly, encode it in CI/test tooling.

## 6) Increase agent legibility, not instruction volume

Invest in making state observable to agents:

- deterministic benchmark scripts with committed outputs,
- structured smoke scripts and stable logs,
- source-anchored design docs,
- explicit ownership and directory maps.

For performance work:

- publish benchmark deltas as committed artifacts and markdown summaries,
- keep workloads long enough to reduce noise.

## 7) Throughput-friendly merge strategy with guardrails

Fast iteration is fine only if correctness/perf/compatibility are gated.

Use:

- small PRs,
- strict smoke/perf gates,
- quick follow-up fixes when needed,
- frequent checkpoint tags once full validation passes.

## 8) Continuous cleanup (garbage collection)

Agent-generated repos drift unless cleaned continuously.

Run recurring maintenance tasks:

- doc gardening: stale constraints, broken links, outdated file references,
- benchmark artifact pruning and normalization,
- codemod/cleanup tasks for repeated anti-patterns.

Treat cleanup as routine maintenance, not occasional heroics.

## 9) Project-specific adoption checklist

Use this checklist for each new subsystem in the clean rebuild:

1. Add/extend scoped docs before implementation.
2. Add at least one targeted smoke test and one perf check.
3. Update execution plan status.
4. Refresh benchmark/report artifacts.
5. Record follow-up debt in the debt tracker.
