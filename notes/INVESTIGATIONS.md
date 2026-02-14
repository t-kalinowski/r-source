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

## 2026-02-14: Serial tax root-cause narrowing (core eval state representation)

### Goal
- Pin down the dominant source of the persistent ~1.45-1.50x serial slowdown in interpreter-heavy loops.

### Baseline (reference vs MTL)
- Reference: `reference-r-devel-clean/build-ref-shlib/bin/R`
- MTL: `build-mtl-shlib/bin/R`
- Benchmark: `bench/loop_microdiag.R`
- Representative median ratios (`mtl / ref`):
  - `empty_for`: ~1.48x
  - `scalar_add`: ~1.50x
  - `sin_loop`: ~1.51x
  - `lookup_scalar`: ~1.48x

### Hypotheses tested and outcomes

1. `R_mtl_sync_compat_exports()` in `context.c` hot paths is the main cost
- Diagnostic: temporarily no-op'd `R_mtl_sync_compat_exports()` in `src/main/mtl-compat.c`.
- Result: essentially no change in loop ratios.
- Conclusion: not dominant.

2. `R_Interpreter` accessor branching/function path is the main cost
- Diagnostic: forced `R_Interpreter` to direct `R_InterpreterMain` macro path in `src/include/Defn.h` (serial-only diagnostic).
- Result: essentially no change in loop ratios.
- Conclusion: accessor branch/function itself is not dominant.

3. Memory/allocator MTL guards dominate idle serial tax
- Diagnostic A: forced `R_MTL_THREADING_ACTIVE` to compile-time `0` inside `src/main/memory.c` only.
- Diagnostic B: forced `R_MTL_THREADING_ACTIVE` compile-time `0` globally in `src/include/Defn.h`.
- Result: only small improvement (~3-5% on loop medians), far short of eliminating the ~1.5x gap.
- Conclusion: allocator/runtime guards are a contributor, but not the dominant source.

### Disassembly evidence (bcEval loop)
- Compared `_bcEval_loop` disassembly in `libR.dylib` (MTL vs reference) with:
  - `xcrun llvm-objdump --disassemble-symbols=_bcEval_loop ...`
- Observed:
  - MTL has materially more load/store ops in the loop body (`ldr/str/ldp/stp` count higher than reference).
  - MTL code references interpreter-struct symbols (`_R_Interpreter0` region); reference does not.
- `eval.c` source diff versus reference is minimal; generated code differences are therefore largely from macro/state representation, not semantic algorithm changes in `eval.c`.

### Current conclusion
- Dominant remaining serial tax is from VM hot-state representation and codegen effects after moving eval/runtime state behind per-interpreter struct fields, not from:
  - compat export syncing,
  - simple `R_MTL_THREADING_ACTIVE` guard branches,
  - or the `R_Interpreter` accessor branch itself.

### Next action
- Prototype a main-thread zero-tax layout for VM hot globals (`R_BCNodeStackTop`, `R_BCProtTop`, `R_EvalDepth`, `R_Visible`, etc.) that preserves reference-like codegen when pool is idle, while keeping worker state isolated when threading is active.
- Validate against `bench/loop_microdiag.R` first, then full smoke ladder.

## 2026-02-14: Serial parity push (actual source of README lapply gap)

### Goal
- Reach near-zero serial tax (`lapply`) versus the frozen reference build used in this repo.

### What was diagnosed
- The large serial gap in README workloads was not primarily from MTL allocator/lookup guards.
- A major component came from newer `do_math1` wrapper dispatch introduced in `src/main/arithmetic.c` (commit `e996f33116` in this tree), which changed:
  - `sqrt` -> `Rsqrt`
  - `exp/expm1/log1p/sin/cos/tan/...` -> `math1_ari` wrappers
- These wrappers materially slowed the benchmark workloads that are heavy on `sin/cos/log1p/sqrt`.

### Changes made
1. Restored hot `Math` dispatch in `src/main/arithmetic.c` to reference-style direct `MATH1(...)` for:
   - `sqrt`, `exp`, `expm1`, `log1p`, `cos`, `sin`, `tan`, `acos`, `asin`, `atan`,
   - `cosh`, `sinh`, `tanh`, `acosh`, `asinh`, `atanh`.
2. Kept arithmetic reuse path on pure serial semantics:
   - `src/main/arithmetic.h`: `R_mtl_worker_mode()` returns `0` (with comment).
   - This removes per-op worker ownership reuse checks from serial hot paths.
   - Regression suite (`tests/mtlapply.R`) still passes, including mutation-guard tests.

### Measurements
- Vector-heavy micro (same expression used in ETL workload):
  - Reference median: `0.032s`
  - MTL median after math dispatch rollback: `0.0325s` (near parity)
- README serial `lapply` ratios (`mtl/ref`) after these changes:
  - `alloc_pressure`: `1.0076`
  - `cos_seq`: `0.9912`
  - `etl_group_mean`: `0.9865`
- MTL scaling remained strong on the same build (`bench/results/mtl_final_threads.rds`):
  - ~2x at 2 threads, ~3.4-3.9x at 4 threads, ~6.0-7.7x at 8 threads.

### Conclusion
- The remaining serial gap is now effectively gone for the tracked README workloads against the frozen reference build.
- Main takeaway: apparent "MTL serial tax" in these workloads was largely conflated with the newer `Math` wrapper dispatch behavior in this tree.

## 2026-02-14 (later): persistent serial tax in tight interpreter loops

### Trigger
- Re-checking with tighter scalar-loop diagnostics showed a remaining serial tax that is larger than the README medians suggested.

### Repros
- Reference (`reference-r-devel-clean/build-ref-shlib/bin/R`) vs MTL (`build-mtl-shlib/bin/R`) medians:
  - `for(i in 1:4e7) s <- s + i`:
    - ref: `~0.356s`
    - mtl: `~0.458s`
  - `for(i in 1:2e7) x <- x + sin(i)`:
    - ref: `~0.442s`
    - mtl: `~0.501s`
  - alloc-heavy micro (`alloc_small`):
    - ref: `~0.609s`
    - mtl: improved from `~0.80s` down to `~0.742s`, still slower.

### Changes tried
1. `src/main/memory.c`
- `mtl_alloc_use_serial_fastpath()`:
  - switched from TLS-only check to:
    - fast return when `!R_MTL_THREADING_ACTIVE`
    - worker check only when active.

2. `src/include/Defn.h`
- `R_mtl_interpreter_ptr()`:
  - fast path now returns `R_InterpreterMain` when `!R_MTL_THREADING_ACTIVE`
  - TLS path moved to `R_mtl_interpreter_tls_or_main()` only when active.

3. `src/main/envir.c`
- reordered shared-env guard helpers to return immediately when threading inactive, before envstats checks:
  - `mtl_worker_shared_env_access()`
  - `mtl_main_shared_env_mutation()`
  - `mtl_main_shared_searchpath_mutation()`

### Direct checks
- Verified `R_mtl_threading_active == 0` at startup using a tiny `inline::cfunction`.
- Pool state at startup is idle (`threads.current=0`, `job.active=0`).
- `tests/mtlapply.R` still passes after these edits.

### Current status
- Some improvement in alloc-heavy serial microbenchmarks, but serial parity is still not met for very tight scalar loops.
- README serial parity currently hovers around ~4-5% slower on the latest quick run.

### Working hypothesis
- Remaining tax is dominated by generic interpreter hot-loop overhead under the per-interpreter state model (not just allocator gates), with tight scalar loops amplifying that overhead.

## 2026-02-14 (latest): interpreter-pointer dispatch is a first-order serial cost

### What I tested
1. Ran `bench/loop_microdiag.R` on reference and current MTL build.
- MTL remained ~1.29x-1.33x slower on tight loop cases.

2. Controlled diagnostic patch in `src/include/Defn.h`:
- Temporarily forced `R_mtl_interpreter_ptr()` to always return `R_InterpreterMain`.
- Rebuilt and reran tight-loop spot checks.

### Result
- Serial loop times improved materially under forced-main-pointer mode, confirming interpreter-pointer dispatch is a major contributor to serial tax.
- But even forced-main-pointer did not fully close gap, so there is additional residual overhead beyond that accessor branch.

### Failed experiment (rolled back)
- Tried a narrow fast-mirror of `R_Visible`/`R_EvalDepth` with sync on `R_mtl_set_threading_active()` transitions.
- This introduced startup/runtime corruption (spurious startup printing and unstable behavior).
- Fully rolled back; build restored to stable behavior.

### Safe changes kept
- `envir.c` worker-check cleanup remains:
  - early inactive returns,
  - central `mtl_is_worker_thread()` helper,
  - serial fast exits in define/set paths.

### Next direction
- To use the user idea safely (switch evaluator mode at interrupt checkpoints), we need an explicit dual-evaluator/dual-hotpath design that avoids per-access branching in serial mode without reintroducing unsafe state mirroring.

## 2026-02-14 (latest): hidden TLS mirror for interpreter pointer

### Change
- Added a hidden TLS mirror of the interpreter pointer in `src/include/Defn.h`:
  - visible export kept for bundle ABI: `R_InterpreterTLS`
  - hidden hot-path mirror for main executable code: `R_InterpreterTLS_hidden`
- Added helpers:
  - `R_mtl_interpreter_tls_current()`
  - `R_mtl_interpreter_tls_set()`
- Updated worker enter/exit in `src/main/apply.c` to set both visible+hidden TLS pointers.
- Updated `src/main/mtl-interpreter.c` to read via `R_mtl_interpreter_tls_current()`.

### Why
- Core runtime is built in the main executable; default-visibility TLS can carry interposition overhead.
- The hidden mirror lets internal hot paths read TLS through a hidden symbol while preserving external compatibility for bundles built with dynamic lookup.

### Observed impact
- Serial parity improved materially versus `reference-r-devel-clean/build-ref-shlib` on repeated checks:
  - tight scalar loops moved closer to parity,
  - serial diagnostic workloads improved from earlier ~20%+ deltas down into roughly low-double-digit deltas in the same run window.
- Allocation-heavy kernels are still slower than reference, so this is progress but not the end-state.

### Diagnostic result on allocator gate
- Forcing `mtl_alloc_use_serial_fastpath()` to always return `1` gave only a small improvement on `alloc_small` (~2-3%), so the remaining gap is not primarily that branch.

### Current hypothesis
- Remaining serial tax is largely in allocation-heavy interpreter paths (`allocVector`/vector-expression loops), likely from the broader per-interpreter-state model and allocator indirections, not just one mode check.

## 2026-02-14: serial parity pass focused on protect fast path + arithmetic identity

### Goal
Reduce serial tax in tight kernels while keeping mtl runtime behavior unchanged.

### Changes tested
1. `src/include/Defn.h`
- Kept `R_MTL_INTERNAL` inline PROTECT/UNPROTECT path.
- Added explicit serial fast branch in inline protect helpers that directly updates `R_Interpreter0.ppStackTop/ppStack` when `!R_MTL_THREADING_ACTIVE`.

2. `src/main/arithmetic.[ch]`
- Re-aligned binary/unary allocation/reuse logic with reference implementation semantics.
- Replaced extended `do_math1()` wrapper mapping with baseline `MATH1(...)` dispatch behavior.

3. Build-flag experiment (failed, reverted)
- Temporarily removed `-DR_MTL_INTERNAL` from `src/main/Makefile.in` and `src/main/Makefile.win`.
- Result: significantly worse serial kernels.
- Reverted and rebuilt with `-DR_MTL_INTERNAL` restored.

### Measurements
Using `bench/serial_minimal_bench.R` vs `reference-r-devel-clean/build-ref-shlib`:
- Best current (after protect fast-path and arithmetic alignment):
  - `empty_for_8e7`: ~0.96x to 1.05x (near parity)
  - `scalar_add_4e7`: ~1.01x to 1.09x (near parity)
  - `vector_alloc_64_x_2e6`: ~1.01x to 1.02x (near parity)
  - `alloc_small_sum_x_6e5`: ~1.11x to 1.12x (slightly above threshold)
  - `vector_list_build_x_6e5`: ~1.25x to 1.28x (remaining hotspot)

### Diagnostic conclusion
- Remaining serial gap is no longer broad/global; it is concentrated in arithmetic-heavy vector expression loops.
- The `-DR_MTL_INTERNAL` build path is required for best current serial performance.
- Next focused target should be reducing per-op overhead in repeated binary math expression evaluation (without regressing worker safety).

## 2026-02-14 (latest): reproduce + recover runtime crash, then tighten serial hot path

### Correctness recovery
- Reproduced worker crash from local dirty-tree experiments (`tests/mtlapply.R` failed early with recursive errors / bus error).
- Root cause was unstable local runtime edits (not `HEAD`): restored critical runtime files to baseline and revalidated.
- `build-mtl-shlib/bin/R --vanilla -q -f tests/mtlapply.R` now passes cleanly again.

### Serial-path experiments after recovery
Goal: reduce idle-threadpool serial tax on minimal kernels.

1. `envir.c` fast inactive exits
- Added early returns in:
  - `defineVar()` -> direct `defineVar_impl()` when `!R_MTL_THREADING_ACTIVE`
  - `setVarInFrame()` -> direct `setVarInFrame_impl()` when `!R_MTL_THREADING_ACTIVE`
- Rationale: avoid worker/shared-env gating overhead entirely on serial path.

2. `Defn.h` / interpreter TLS access tuning
- Added hidden TLS mirror for main executable hot path:
  - `R_InterpreterTLS_hidden`
  - synchronized via `R_mtl_interpreter_set()` from worker enter/exit in `apply.c`.
- Marked interpreter pointer helper/setter `always_inline`.
- Set `R_Interpreter` macro directly to hidden/visible TLS symbol (`__MAIN__` vs non-main).
- Tried `tls_model("initial-exec")` for interpreter TLS declarations.

### Measured outcome
Using:
- MTL: `build-mtl-shlib/bin/R`
- baseline: `/usr/local/bin/R-devel`
- command: `bench/serial_minimal_bench.R` + `tools/mtl-serial-minimal-check.R`

Latest ratios (`mtl / baseline`):
- `empty_for_8e7`: `1.176`
- `scalar_add_4e7`: `1.239`
- `vector_alloc_64_x_2e6`: `0.850`
- `vector_list_build_x_6e5`: `0.664`
- `alloc_small_sum_x_6e5`: `0.931`

Interpretation:
- Tight scalar loop tax improved somewhat versus earlier same-session runs, but remains above the 1.10 guardrail.
- Allocation/vector-heavy kernels are still at or faster than baseline in this benchmark.
- Remaining gap is concentrated in tight interpreter-loop kernels (`empty_for`, `scalar_add`).

### Current hypothesis
- The remaining serial tax is dominated by per-op interpreter-state access cost in tight loop execution, not shared-env gating.
- Further progress likely requires a deeper “serial evaluator mode” approach (avoiding TLS-based interpreter indirection on hot serial paths), rather than additional micro-gates in env/allocator wrappers.

## 2026-02-14 (latest): allocation-heavy serial gap vs local reference, with targeted hot-path attempts

### Baseline re-check (apples-to-apples)
- Compared against in-repo reference build (`reference-r-devel-clean/build-ref-shlib/bin/R`), not `/usr/local/bin/R-devel`.
- Tight scalar loops are near parity:
  - `empty_for_8e7`: ~1.00x
  - `scalar_add_4e7`: ~1.00x
- Allocation-heavy minimal kernels remain slower:
  - `vector_alloc_64_x_2e6`: ~1.19x
  - `alloc_small_sum_x_6e5`: ~1.26x
  - `vector_list_build_x_6e5`: ~1.34x

### Focused microdiagnostics
- A minimal vector-expression loop (`for(i in 1:6e5) v <- (i/10) + 1:64`) remains much slower:
  - reference median ~`0.107s`
  - mtl median ~`0.163-0.165s`
- Component splits show broad slowdown across small primitive/allocation steps:
  - `v <- 1:64`, `v <- i/10`, `v <- (i/10)+1`, and `v <- (i/10)+(1:64)` all regress similarly.

### Changes tested this round
1. `arithmetic.[ch]`: serial fast branch in reuse helpers
- Added explicit inactive-threading fast path in `R_allocOrReuseVector()`, `ScalarValue1/2()`, unary minus reuse.
- Diagnostic forced-`R_mtl_can_reuse_object()==1` test showed negligible improvement.
- Conclusion: reuse-ownership checks are not the dominant source of this regression.

2. Hidden symbol routing for core runtime code
- Added `-DR_MTL_CORE` to `src/main/Makefile.in`.
- Switched `Defn.h` hot-path macros (`R_MTL_THREADING_ACTIVE`, `R_Interpreter`/TLS helpers) to use hidden symbols under `R_MTL_CORE`.
- Verified via `nm` that core objects now reference `_R_InterpreterTLS_hidden`.
- Effect on hot vector loop: small improvement (~`0.179` -> `0.165`), far from parity.

3. Protect-stack serial fast path in `memory.c`
- Added explicit inactive-threading fast path in `protect()`, `unprotect()`, `unprotect_ptr()`, `Rf_isProtected()`, `R_ProtectWithIndex()`, `R_Reprotect()` using `R_Interpreter0`.
- Effect: marginal additional improvement (`~0.165` -> `~0.163` median on the hot vector loop).

### Validation
- `tests/mtlapply.R` passes after these changes.
- `tools/mtl-perf-smoke.sh build-mtl-shlib ./reference-r-devel-clean/build-ref-shlib/bin/R 1.50` passes.
  - Threaded speedups still strong (threads=8):
    - `matmul_1000_n20`: ~`6.60x`
    - `matmul_100_n20000`: ~`8.03x`

### Current conclusion
- We improved some hot-path symbol access overhead, but the main remaining serial gap is still in allocation-heavy, small-primitive loops.
- The residual cost appears structural (per-interpreter state model on core eval/alloc/protect path), not a single conditional or ownership check.
- Next likely step is a deeper serial-mode specialization for eval/alloc/protect (single-path main-thread globals when pool is idle), rather than more local branch shaving.
