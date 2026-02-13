# Investigation Log

## 2026-02-13: Serial-path slowdown (allocator hot path)

### Goal
- Track down why pure serial workloads are slower in the MTL build versus a clean reference R-devel build.

### Hypotheses tested
- Hot-path runtime stats checks in allocator code add measurable serial overhead.
- Remaining slowdown might come from interpreter-pointer indirection (TLS/global selection).

### What was changed/tested
- Removed runtime-stats accounting from `mtl_alloc_use_serial_fastpath()` in `src/main/memory.c`.
- Added compile-time gating for runtime stats via `R_MTL_RUNTIME_STATS` in `src/main/memory.c`:
  - stats env lookup and counters are now optional at compile time.
  - default build uses `R_MTL_RUNTIME_STATS=0`.
- Ran serial diagnostics and README-style benchmark comparisons:
  - `bench/serial_runtime_diagnose.R`
  - `bench/readme_bench_run.R`
- Tried an interpreter-pointer fast-path experiment in `src/include/Defn.h`:
  - route `R_Interpreter` to `R_InterpreterMain` when threading inactive.
  - result: significant regression in quick serial probes.
  - experiment was reverted.

### Results
- Confirmed allocator-path stats checks were a real contributor to serial tax.
- After allocator changes (with reverted interpreter-pointer experiment), serial gap remains but improved on allocation-heavy probes relative to earlier measurements in this session.
- Current serial deltas versus `reference-r-devel-clean/build-ref-shlib` are still non-zero, especially on allocation-heavy workloads (roughly low-teens to ~20% depending on workload mix/run).

### Regressions / reversions
- Interpreter-pointer experiment in `src/include/Defn.h` regressed serial runtime and was reverted immediately.

### Next action
- Continue reducing serial allocator overhead by making the `!R_MTL_THREADING_ACTIVE` path closer to upstream allocator code paths (minimize MTL-specific helper/branch cost on serial hot paths).
