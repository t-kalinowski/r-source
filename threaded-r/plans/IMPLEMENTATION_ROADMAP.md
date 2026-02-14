# Implementation Roadmap (Clean Rebuild)

## Phase 0: Seed + Guardrails

- [ ] Fork fresh upstream `R-devel` baseline.
- [ ] Add this `threaded-r/` scaffold at repo root.
- [ ] Wire baseline benchmark scripts and dashboard rendering.
- [ ] Capture baseline serial CSVs on target hardware.
- [ ] Define pass/fail thresholds for serial parity and scaling checks.

Exit criteria:

- Bench dashboard renders from CSV inputs.
- CI/local check target exists for benchmark + smoke suite.

## Phase 1: Runtime scaffolding (no user API)

- [ ] Introduce minimal threadpool lifecycle (init, idle, shutdown).
- [ ] Add inert subinterpreter state object(s) without changing eval behavior.
- [ ] Add runtime observability counters (pool state, queue depth, active workers).

Exit criteria:

- No behavior changes in serial execution.
- Serial benchmarks remain within parity threshold.

## Phase 2: `mtlapply` core path

- [ ] Add `mtlapply(X, FUN, ...)` with `lapply`-compatible signature.
- [ ] Use `options(threads = n)` only.
- [ ] Implement work queue + worker pull model.
- [ ] Return ordered result list with deterministic error propagation.

Exit criteria:

- Correctness smokes pass.
- Basic threaded speedups observable on reference workloads.

## Phase 3: Isolation + safety policies

- [ ] Enforce worker global-state mutation guardrails.
- [ ] Ensure clear error messages for forbidden operations.
- [ ] Validate traceback/recoverability after worker failures.

Exit criteria:

- Error-path regression tests pass repeatedly.
- No poisoned-session behavior after failures.

## Phase 4: Native/package compatibility

- [ ] Enable worker-safe `.Call`/`.External` execution paths.
- [ ] Add main-thread serialization where required for global runtime mutation points.
- [ ] Add package-load smokes for standard and binary libraries.
- [ ] Add embedded `rsession` smoke.

Exit criteria:

- Representative compiled-package worker calls pass.
- Drop-in package-load smoke suite passes.

## Phase 5: Futures API

- [ ] Implement `background()`.
- [ ] Implement `wait()` with pop-done semantics.
- [ ] Implement `cancel()` best-effort behavior.
- [ ] Ensure futures and `mtlapply` share one persistent pool.

Exit criteria:

- Future correctness tests pass.
- Benchmarks demonstrate throughput/latency gains in async-like workflows.

## Phase 6: Serial parity hardening

- [ ] Run targeted microdiagnostics to locate remaining serial tax.
- [ ] Specialize idle-threadpool hot paths as needed.
- [ ] Keep all threaded correctness/package tests green while tuning.

Exit criteria:

- Serial benchmarks at/near baseline threshold.
- Threaded speedups preserved.

## Continuous checklist (every merge)

- [ ] Runtime correctness smoke.
- [ ] Package ABI/load smoke.
- [ ] Serial parity benchmark gate.
- [ ] Threaded scaling benchmark gate.
- [ ] Dashboard artifact refreshed.
