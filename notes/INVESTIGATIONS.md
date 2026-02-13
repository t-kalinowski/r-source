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

## 2026-02-13: Idle serial tax in interpreter-heavy loops

### Goal
- Explain persistent serial slowdown while the threadpool is idle, especially in simple scalar loops.

### What was changed/tested
- Added a worker-ownership fast-path helper in arithmetic code:
  - `src/main/arithmetic.h`: `R_mtl_can_reuse_object()`
  - `src/main/arithmetic.c`: switched scalar reuse checks to use that helper
- Helper short-circuits on `!R_MTL_THREADING_ACTIVE` before worker/heap checks.
- Re-ran serial diagnostics with heavier reps:
  - `bench/serial_runtime_diagnose.R`
  - `bench/render_serial_runtime_diagnose.R`
- Added focused loop microdiagnostic:
  - `bench/loop_microdiag.R`
- Compared `build-mtl-shlib` vs `/usr/local/bin/R-devel` on:
  - empty `for` loop
  - scalar add loop
  - `sin` loop
  - lookup-heavy scalar loop

### Results
- Arithmetic fast-path change produced a small improvement in serial diagnostic ratios for loop-heavy cases (~1-2% absolute ratio improvement).
- However, major idle serial tax remains on interpreter-heavy loops:
  - `empty_for`: ~1.48x slower
  - `scalar_add`: ~1.46x slower
  - `sin_loop`: ~1.47x slower
  - `lookup_scalar`: ~1.44x slower
- Allocation-heavy and README-style parity guard workloads still pass current threshold and often run at parity or faster.

### Current conclusion
- The remaining idle serial tax is dominated by core interpreter-loop overhead, not allocator ownership checks.
- Most likely source is thread-local interpreter-state indirection in very hot eval paths.

### Next action
- Investigate a structural fast path for main-thread interpreter state that removes per-op TLS/indirection cost when `R_MTL_THREADING_ACTIVE == 0`.
- Keep `bench/loop_microdiag.R` as a standing microbenchmark gate for this specific issue.
