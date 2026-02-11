# R MT Subinterpreters / Threads Notes (Working Instructions)

## Overall Goal
Add support for running R code in parallel on multiple OS threads inside one R process, conceptually as subinterpreters (per-thread interpreter state), exposed initially as a very simple interface:

- `mtlapply(X, FUN, ..., threads=)` modelled after `lapply()` / `mclapply()`
- Evaluate `FUN` across multiple threads/subinterpreters
- Transfer results back to the main thread (goal: move/transfer, not serialize/copy)

Key performance requirements from the outset:

- True multi-threaded evaluation of *most R code* (not just native math)
- Avoid a GIL for “normal evaluation” (locks only at boundaries / exceptional global mutations)
- No performance degradation for regular serial execution (e.g. `lapply()` should be parity with baseline R)

## Constraints / Design Requirements
- Each thread should have its own protect stack.
- Each thread should have its own GC/heap, with GCs mostly independent:
  - If a background thread runs GC, it should not pause the main thread or other background threads.
- “Workers are in a read-only world” w.r.t. global state:
  - Workers should read global/shared objects but not mutate them.
  - Workers should allocate without blocking, but only for closure locals, temporaries, and return values.
- Some operations that might modify global state may be allowed from workers only under a lock (e.g. things analogous to `options()` setters, `tempfile()`, package loading).

## Development Style (Requested)
- Staged approach; commit early and often.
- Full test suite is too slow to run every commit:
  - Run minimal checks / incremental builds frequently.
  - Run the full test suite occasionally at checkpoints.
- Starting state should be clean on `main`/`trunk`; if there are unstaged changes they can be discarded.
- Avoid “try-then-fallback” patterns: prefer one happy path; fail fast if assumptions don’t hold.
- Avoid excessive argument checking in duck-typed R/Python:
  - A small handful of `stopifnot()` checks is OK, but no exhaustive fallback trees.

## Package / C API Story (Requested Direction)
Work towards making this build of R usable for package code that calls `.Call()` and related native entry points:

- Allow a subset of the C API to be safely callable from worker threads.
- Loading packages may require a lock / must run on the main thread initially.
- Allocation APIs should work from workers; introspection APIs for reading values out of `SEXP` should work.
- Long term: ability to install and run CRAN packages under this build.

## Iteration Workflows That Were Productive Here
### Build/Run Loops
- Maintain separate build directories for faster iteration (e.g. `build-mtl`, `build-mtl-clang`).
- Rebuild incrementally with `make -j`.
- On macOS, relinking often requires re-signing the `R` executable:
  - `codesign --force --sign - build-*/bin/exec/R`

### Current Default Build Target (for ABI-compatible package smoke tests)
- Use a shared-lib build for binary-package ABI testing:
  - `build-mtl-shlib` configured with `--enable-R-shlib`
- After each relink in `build-mtl-shlib`, re-apply install-name fixups:
  - `bash tools/mtl-abi-macos.sh build-mtl-shlib`
- Reason: existing macOS binary packages are linked against framework install names; this avoids loading a second `libR.dylib`.

### Benchmarks and Regression Checks
- Compare against the system development build at `/usr/local/bin/R-devel` for apples-to-apples timing.
- Run heavier benchmarks (larger workloads, more iterations) to reduce measurement noise.
- Persist benchmark results as artifacts (e.g. `.rds`) and load/compare them later.

### Process Management During Benchmarking
- Prefer running long or kill-prone commands via the MCP console:
  - Spawn work with `system2()` inside the console session.
  - If something wedges or spawns children, use `manage_session(\"restart\")` to cleanly kill the session and its children, then continue.

## Default Validation Ladder (Run in This Order)
Use this as the standard iteration checklist after runtime changes.

1. **Incremental rebuild**
   - `make -C build-mtl-shlib -j8`
   - `bash tools/mtl-abi-macos.sh build-mtl-shlib`

2. **Core threaded runtime smoke**
   - `build-mtl-shlib/bin/R --vanilla -q -f tests/mtlapply.R`

3. **Binary package ABI smoke (minimal)**
   - `build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-abi-smoke.R --args /Users/tomasz/Library/R/arm64/4.6/library`
   - Confirms existing compiled packages (e.g. `digest`, `Rcpp`) load in this build.

4. **Binary package ABI smoke (real workflow via dplyr)**
   - `tools/mtl-dplyr-smoke.sh build-mtl-shlib /Users/tomasz/Library/R/arm64/4.6/library 4`
   - Confirms package load and representative main-thread dplyr/tibble flows.
   - Optional worker-side dplyr check is opt-in:
     - `MTL_DPLYR_WORKER=1 build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dplyr-smoke.R --args /Users/tomasz/Library/R/arm64/4.6/library 4`

5. **Worker-native package smoke (`.Call`-heavy paths)**
   - `build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-worker-native-smoke.R --args /Users/tomasz/Library/R/arm64/4.6/library 4 64`
   - Confirms package code that relies on native entry points behaves identically under `lapply()` and `mtlapply()` for representative workflows.
   - Also emits `.Internal(mtlrpcstats(FALSE))` for quick visibility into worker->main fallback pressure.

6. **Benchmark checkpoint (always include in flow)**
   - Generate timing artifacts:
     - System R:
       - `/usr/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/system.rds`
     - R-devel:
       - `/usr/local/bin/R-devel --vanilla -q -f bench/readme_bench_run.R --args bench/results/rdevel.rds`
     - MTL build:
       - `build-mtl-shlib/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl.rds`
   - Optional richer benchmark object:
     - `build-mtl-shlib/bin/R --vanilla -q -f bench/readme_bench_mark.R --args bench/results/bench_mark.rds`
   - Refresh human-readable report:
     - `build-mtl-shlib/bin/R --vanilla -q -e 'rmarkdown::render(\"README.Rmd\", output_format = \"github_document\")'`

7. **Interpretation rule**
   - Check serial parity first (`lapply` path in MTL build vs R-devel/system R).
   - Then check scaling (`mtlapply(2/4/8)` vs `mtlapply(1)` and `lapply`).
   - Treat benchmark noise seriously: prefer larger workloads and repeated runs before concluding regressions.

## Package Compatibility Goal (Current Concrete Target)
- Short term target: this build should load and run already-built CRAN binaries (no package code changes required).
- Required for each checkpoint:
  - ABI smoke (`tools/mtl-abi-smoke.R`)
  - dplyr smoke (`tools/mtl-dplyr-smoke.R`)
  - worker-native smoke (`tools/mtl-worker-native-smoke.R`)
- Any regression here blocks progress, even if internal microbenchmarks improve.

## Active Execution Plan (Current)
- [x] Add worker->main RPC reason counters and expose stats (`.Internal(mtlrpcstats)`).
- [x] Add package smoke wrapper (`tools/mtl-package-smoke.sh`) that runs ABI + dplyr checks.
- [x] Add dedicated worker-native package smoke for `.Call`-heavy paths.
- [ ] Expand worker-native smoke to more compiled packages (as available in system library).
- [ ] Keep serial benchmark parity at each checkpoint before increasing worker coverage.

## Notes / Questions to Keep In Mind
- Serial parity is non-negotiable: any added checks/atomics/dispatch on hot serial paths must be avoided.
- Symbol resolution should be fast in all threads:
  - If the symbol already exists in the symbol table, resolving it should not require a global lock (or should be extremely lightweight).
- R does not have true refcounting (only a few bits in the header), so cross-heap ownership/transfer needs an approach that does not depend on unbounded refcounts.
- `.Call` / `.External` compatibility is part of baseline package usability; package authors should not need a new registration model.
- Loading packages can stay main-thread-only initially, but calling already-registered routines must remain transparent.
