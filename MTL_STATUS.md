# MTL Status: Threaded Subinterpreters in R (Session Summary)

This document summarizes the current state of the multi-threaded interpreter (`mtlapply`) work in this repo, focusing on what changed during this effort, what works now, how it is designed, what remains limited, and where to go next.

## 1) Goals and Scope

The project goal has been to make parallel R evaluation possible inside one process, via multiple interpreter states (subinterpreters) and a simple entry point:

- `mtlapply(X, FUN, ..., threads = n)`

The guiding constraints have been:

- true multi-threaded execution (not fork-based)
- worker-local protection / allocation / GC behavior where possible
- practical package compatibility (especially `.Call`/`.External` usage)
- serial workloads must not regress versus standard R builds

The work here is experimental, but it is no longer just a toy. It now includes a persistent thread pool, worker interpreter state, a compatibility layer for worker->main fallbacks, and a concrete smoke/benchmark workflow.

## 2) What Was Implemented

### 2.1 User-facing API and runtime entry points

- Added `mtlapply` in base:
  - `src/library/base/R/mtlapply.R`
  - `src/library/base/man/mtlapply.Rd`
  - internal entry in `src/main/names.c` / `src/main/apply.c`

- Added thread-pool backed execution path with persistent workers:
  - `src/main/apply.c`

### 2.2 Per-interpreter state and worker identity

A major refactor moved more behavior away from implicit global assumptions and into interpreter-specific state and thread-local selection:

- thread-local `R_Interpreter` usage
- worker identity flag (`isMTLWorker`) in interpreter state
- per-worker global environment behavior for `mtlapply` evaluation
- worker-specific handling in evaluator/environment paths where needed

Representative files touched:

- `src/include/Defn.h`
- `src/main/main.c`
- `src/main/eval.c`
- `src/main/envir.c`
- `src/main/apply.c`

### 2.3 Memory/GC direction: multi-heap model

The runtime now carries a concrete multi-heap direction rather than a pure global-heap-only approach:

- interpreter heap state plumbing
- worker heap initialization/adoption paths
- owner-aware bookkeeping for some allocation paths
- root scanning updates for multiple interpreters/heaps
- GC trigger/flag changes to reduce cross-thread interference

Representative files:

- `src/main/memory.c`
- `src/include/Defn.h`

This is not yet a fully independent no-contention heap/GC architecture for all operations, but it is far beyond a single coarse lock around all evaluation.

### 2.4 Worker safety policy and restrictions

To prevent undefined behavior while enabling useful concurrency, targeted restrictions and compatibility behavior were introduced:

- superassignment from workers is rejected
- `dyn.load` / `dyn.unload` disallowed from worker threads
- weakrefs/finalizers in workers are restricted
- selected operations are routed back to main thread when required

Representative locations:

- `src/main/eval.c`
- `src/main/Rdynload.c`
- `src/main/memory.c`

### 2.5 Package-native code path: from global lock to compatibility RPC

This session advanced the package story from “native code must serialize” toward “native code can run on workers, with explicit fallback for thread-sensitive internals.”

Key step:

- worker->main RPC mechanism in `src/main/apply.c` (`R_mtl_invoke_on_main*`)
- enables worker execution of `.Call/.External` while routing selected internal operations to main thread

Then expanded with reasoned telemetry:

- `R_mtl_invoke_on_main_reason(...)`
- per-reason counters (`install`, `installNoTrChar`, `mkCharLenCE`, `parseVector`, `parseConn`, `doParse`, `other`)
- `.Internal(mtlrpcstats(reset))` for observability

Touched files include:

- `src/include/Defn.h`
- `src/include/Internal.h`
- `src/main/apply.c`
- `src/main/names.c`
- `src/main/envir.c`
- `src/main/gram.c`
- `src/main/source.c`

This compatibility layer is central to maintaining ABI compatibility while still allowing worker-side native execution for many practical package paths.

## 3) Package / ABI Compatibility Progress

A major focus has been to make this build usable with already-built packages (no package-author code changes).

### 3.1 macOS ABI loading compatibility

Added tooling to run this build against framework-linked binary packages:

- `tools/mtl-abi-macos.sh`
- `tools/mtl-abi-smoke.R`

This addresses install-name/dylib loading realities on macOS so packages can be loaded by this build without rebuilding everything first.

### 3.2 dplyr and tidyverse-adjacent smoke coverage

Added and evolved:

- `tools/mtl-dplyr-smoke.R`
- `tools/mtl-dplyr-smoke.sh`
- `tools/mtl-package-smoke.sh`

Current behavior:

- default dplyr smoke validates package load and representative main-thread workflows
- worker-side dplyr execution is available as an opt-in (`MTL_DPLYR_WORKER=1`) because this is still an active compatibility frontier

### 3.3 Worker-native package smoke

Added a dedicated worker-native smoke test focused on common `.Call`-heavy paths that are expected to work well today:

- `tools/mtl-worker-native-smoke.R`

It validates that `lapply` and `mtlapply` results match for representative worker execution using packages such as:

- `digest`
- `vctrs`
- `rlang`

and prints `mtlrpcstats` so we can see fallback pressure.

## 4) Performance and Benchmarking

### 4.1 Benchmark harness and README artifacts

Benchmark flow now includes:

- system R baseline
- `/usr/local/bin/R-devel` baseline
- this MTL build

Scripts:

- `bench/readme_bench_run.R`
- `bench/readme_bench_mark.R`
- `README.Rmd` -> `README.md`

### 4.2 Current result profile

From the latest run in this session (README artifacts refreshed):

- serial (`lapply`) is near parity with `R-devel` across tested workloads
- strong scaling for compute/ETL-style workloads under `mtlapply`
- allocation-heavy workloads scale, but less cleanly and with lower efficiency

This matches the current architecture: worker parallelism helps when work is sufficiently compute-heavy and contention-sensitive internals are not dominating.

### 4.3 Serial-overhead work already done

Multiple changes specifically targeted “no serial tax”:

- fast-path gating so threaded machinery is inactive outside `mtlapply`
- reduced TLS/interpreter-selection overhead in serial path
- reduced ownership/bookkeeping cost in serial mode
- improved behavior when GC is disabled / heap growth under stress

These changes were necessary to keep ordinary single-threaded code competitive.

## 5) Tests and Validation Added

Representative test additions and updates:

- `tests/mtlapply.R`
  - core `mtlapply` behavior
  - parallel overlap check
  - package/native smoke via internal test package (`tests/Pkgs/mtlPkg`)
- `tests/mtlstress.R`
- `tests/mtlbench.R`
- routing and restriction checks for worker context edge cases

Operational validation ladder (codified in `AGENTS.md`) now includes:

1. incremental build
2. core `tests/mtlapply.R`
3. ABI smoke (`tools/mtl-abi-smoke.R`)
4. dplyr smoke
5. worker-native package smoke
6. benchmark checkpoint + README artifact refresh

## 6) Design Approach (Current)

The current design is pragmatic and layered:

1. **Run worker code truly in parallel where safe**
2. **Route known thread-sensitive internals to main thread via RPC**
3. **Measure fallback pressure (`mtlrpcstats`) and reduce it over time**
4. **Keep serial fast path clean when not inside `mtlapply`**

This avoids a blanket global interpreter lock during all evaluation, while still preserving correctness around internals that are not yet worker-safe.

In other words: parallel-by-default inside `mtlapply`, compatibility fallbacks where required, and instrumentation-driven hardening.

## 7) Known Limitations and Gaps

Despite significant progress, this is still experimental. Current limitations include:

- not all of R internals are worker-safe yet
- some operations still require worker->main fallback (symbol/char interning, parse-related paths, etc.)
- worker-side dplyr paths can still be flaky in specific scenarios (hence opt-in gating for that smoke path)
- restrictions remain for certain worker actions (`dyn.load`, specific mutation patterns, weakrefs/finalizers)
- no guarantee yet that “any CRAN package + any test suite” passes unchanged

Also, independent-GC ambitions are only partially realized in practice today. Heap-local behavior has advanced, but complete no-contention independence for all realistic package workloads is not done.

## 8) What Changed Most Recently (This Session End-State)

Most recent milestone commits:

- `bf3983f614`:
  - reason-tagged worker->main RPC counters
  - `.Internal(mtlrpcstats(reset))`
  - expanded package smoke suite plumbing

- `1843b110a5`:
  - added `tools/mtl-worker-native-smoke.R`
  - stabilized validation ladder (`tools/mtl-package-smoke.sh`)
  - gated worker dplyr smoke behind `MTL_DPLYR_WORKER=1`
  - refreshed README benchmark artifacts

## 9) Recommended Next Steps

Highest-value next work, in order:

1. **Broaden worker-native package smoke coverage**
   - add more compiled packages from the system library to `tools/mtl-worker-native-smoke.R`
   - keep these as required checkpoint gates

2. **Reduce fallback frequency**
   - use `mtlrpcstats` to identify hot fallback reasons under real package workloads
   - make high-frequency internals worker-safe where feasible

3. **Harden worker-side dplyr/tidy-eval paths**
   - remove the need for `MTL_DPLYR_WORKER=1` gating over time

4. **Continue serial-parity enforcement**
   - keep benchmark checkpoints mandatory for runtime/internal changes
   - treat regressions in `lapply` path as blocking

5. **Clarify worker-safe C API subset policy in docs**
   - document current guarantees and fallback behavior in one place for maintainers

## 10) Bottom Line

The project has moved from prototype to a functioning experimental runtime:

- `mtlapply` executes real workloads in parallel with significant speedups in favorable cases
- package compatibility story is now concrete (ABI smoke + package-native smoke + compatibility RPC)
- instrumentation exists to drive next optimization/hardening steps
- serial performance has been actively protected and is now close to baseline in the current benchmark set

What remains is mostly “coverage and hardening”: extending worker-safe internal behavior and package compatibility breadth until this can be treated as robust for general package ecosystems.
