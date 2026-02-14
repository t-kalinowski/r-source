# Test Matrix (Must-Have)

## A. Core correctness

- `mtlapply` basic output equivalence to `lapply` for pure closures.
- deterministic ordered results.
- nested `mtlapply` closure-environment correctness.
- worker error propagation and clean session recovery.

## B. Safety/guardrails

- worker assignment to `globalenv()` fails cleanly.
- worker process-global mutation (`setwd`, similar) fails cleanly.
- closure-local mutation remains allowed.

## C. Native/package behavior

- worker `.Call` path on loaded package routines.
- package load/register smoke (main-thread serialized where required).
- standard library + binary library namespace load sweeps.

## D. Future API behavior

- `background` enqueues and returns future handle.
- `wait` returns next completed future from a set.
- `cancel` is safe and deterministic for queued jobs.

## E. Performance gates

- serial parity workloads (must stay under configured ratio threshold).
- threaded scaling workloads (`threads=2/4/8`).
- allocation-heavy + eval-heavy + native-heavy workload mix.

## F. Harness and knowledge-system checks

- `AGENTS.md` remains concise and map-style.
- non-trivial work has an execution plan in `docs/exec-plans/active` or `docs/exec-plans/completed`.
- key docs are cross-linked and present (`THREADING_PRODUCT_DIRECTION`, invariants, roadmap).
- benchmark dashboard can be regenerated from committed CSV inputs.
