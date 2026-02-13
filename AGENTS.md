# R MT Subinterpreters / Threads Notes (Working Instructions)

## Overall Goal
Add support for running R code in parallel on multiple OS threads inside one R process, conceptually as subinterpreters (per-thread interpreter state), exposed initially as a very simple interface:

- `mtlapply(X, FUN, ...)` modelled after `lapply()` / `mclapply()`
- thread-pool size is controlled via `options(threads = n)`
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
- Keep `git status` meaningful:
  - Ignore generated/local artifacts that create noisy untracked diffs.
  - Before handoff, ensure remaining diffs are intentional source/test/doc changes.
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
- Maintain separate build directories for faster iteration (e.g. `build-mtl-shlib`, `build-mtl-clang`).
- Rebuild incrementally with `make -j`.
- Canonical configure entrypoint:
  - `tools/mtl-configure.sh build-mtl-shlib -- --enable-R-shlib --without-x --disable-java --without-recommended-packages ...`
- For full upstream-style validation before toolchain experiments (e.g. gcc vs clang), use a dedicated build dir with recommended packages enabled and run:
  - `make -C <build-dir> check-all`
  - If `configure` reports missing recommended tarballs, fetch with `tools/fetch-recommended`.
- On macOS, relinking often requires re-signing the `R` executable:
  - `codesign --force --sign - build-*/bin/exec/R`

### Current Default Build Target (for ABI-compatible package smoke tests)
- Use a shared-lib build for binary-package ABI testing:
  - `build-mtl-shlib` configured with `--enable-R-shlib`
- Repo default now enforces this via `config.site`:
  - `enable_R_shlib=yes`
  - so plain `../configure ...` in build dirs will build shared `libR` unless explicitly overridden with `--disable-R-shlib`.
- On macOS, keep configure/build local first, then apply framework ABI install-name mapping post-build:
  - `bash tools/mtl-abi-macos.sh build-mtl-shlib`
  - (avoids linking the build-time `R` executable directly against the system framework `libR`)
- Default package-library behavior in this tree:
  - `R_LIBS_USER` defaults to `'%S-mtl:%U'` so the build-local library is used first,
    then the normal user library fallback (`~/Library/R/<arch>/<x.y>/library` on macOS).
- On macOS shared-lib builds, `src/library/profile/Rprofile.unix` appends the matching
  framework library path (`/Library/Frameworks/R.framework/Versions/<x.y>-<arch>/Resources/library`)
  so framework-linked binaries (e.g. `Matrix`) are found automatically.
- `etc/ldpaths` prepends `${R_HOME}/lib` to both `DYLD_FALLBACK_LIBRARY_PATH` and
  `DYLD_LIBRARY_PATH` on macOS so framework-encoded package dependencies resolve to
  this build's local `libR/libRblas/libRlapack` first (prevents loading a second `libR` image).
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
- For direct shell runs outside MCP console, wrap risky/experimental repros in
  `gtimeout` (macOS coreutils) so hangs self-terminate:
  - `/opt/homebrew/bin/gtimeout 30 build-mtl-shlib/bin/R --vanilla -q -f <script.R>`

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

5. **Drop-in binary package smoke (macOS framework libs)**
   - `build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dropin-smoke.R --args 4`
   - Confirms `Matrix`, `reticulate`, `dplyr`, and a basic `mtlapply()` worker path.

6. **RStudio `rsession` smoke (no GUI)**
   - `tools/mtl-rsession-smoke.sh build-mtl-shlib`
   - Launches `rsession` directly against this build via `DYLD_INSERT_LIBRARIES`
     and verifies startup wiring for `RSTUDIO_WHICH_R`/`R_HOME`.
   - In restricted sandboxes where socket/listener setup is blocked by policy,
     this reports a skip instead of failing.

7. **Worker-native package smoke (`.Call`-heavy paths)**
   - `build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-worker-native-smoke.R --args /Users/tomasz/Library/R/arm64/4.6/library 4 64`
   - Confirms package code that relies on native entry points behaves identically under `lapply()` and `mtlapply()` for representative workflows.
   - Also emits `.Internal(mtlrpcstats(FALSE))` for quick visibility into worker->main fallback pressure.

8. **Standard-build package load sweep**
   - `tools/mtl-load-standard-library-smoke.sh build-mtl-shlib build-mtl-shlib/library`
   - Confirms this build can load all package namespaces from the standard built package set (base/recommended in `build-*/library`).
   - For compiled packages, also exercises minimal native runtime paths by inspecting registration tables and resolving representative registered symbols in isolated child processes.

9. **Framework-binary package load sweep (macOS)**
   - `tools/mtl-framework-library-smoke.sh build-mtl-shlib /Library/Frameworks/R.framework/Versions/4.6-arm64/Resources/library`
   - Confirms in-tree MTL build can load prebuilt framework package binaries without loading a second `libR`.

10. **Benchmark checkpoint (always include in flow)**
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

11. **Serial regression guard**
   - `tools/mtl-perf-smoke.sh build-mtl-shlib /usr/local/bin/R-devel 1.10`
   - Fails if `lapply` median runtime in MTL build exceeds baseline by more than threshold (default `1.10`).

12. **Interpretation rule**
   - Check serial parity first (`lapply` path in MTL build vs R-devel/system R).
   - Then check scaling (`mtlapply(2/4/8)` vs `mtlapply(1)` and `lapply`).
   - Treat benchmark noise seriously: prefer larger workloads and repeated runs before concluding regressions.
   - Convenience wrapper for 3-9:
    - `tools/mtl-validation-smoke.sh build-mtl-shlib /Users/tomasz/Library/R/arm64/4.6/library 4 build-mtl-shlib/library /usr/local/bin/R-devel 1.10`

## Package Compatibility Goal (Current Concrete Target)
- Short term target: this build should load and run already-built CRAN binaries (no package code changes required).
- Required for each checkpoint:
  - ABI smoke (`tools/mtl-abi-smoke.R`)
  - dplyr smoke (`tools/mtl-dplyr-smoke.R`)
  - RStudio `rsession` smoke (`tools/mtl-rsession-smoke.sh`)
  - worker-native smoke (`tools/mtl-worker-native-smoke.R`)
  - standard-build library load sweep (`tools/mtl-load-standard-library-smoke.sh`)
  - serial performance guard (`tools/mtl-perf-smoke.sh`)
- Any regression here blocks progress, even if internal microbenchmarks improve.

## Active Execution Plan (Current)
- [x] Add worker->main RPC reason counters and expose stats (`.Internal(mtlrpcstats)`).
- [x] Add package smoke wrapper (`tools/mtl-package-smoke.sh`) that runs ABI + dplyr checks.
- [x] Add dedicated worker-native package smoke for `.Call`-heavy paths.
- [x] Add standard-build package load sweep for namespace compatibility.
- [x] Add serial performance regression guard with thresholded pass/fail.
- [x] Move `mtlapply` job state to heap-owned lifetime (worker refs + main owner).
- [x] Add fail-fast cancellation path on worker/main error (`cancel_requested` + active-eval quiescence gate).
- [x] Add user-facing regressions for handled worker errors and repeated failure/recovery in `tests/mtlapply.R`.
- [x] Move worker-completion tracking to per-job state (`job->workers_done`) to decouple scheduling state from global pool bookkeeping.
- [x] Add pool-reuse observability (`.Internal(mtlpoolstats)`) and assert nested `mtlapply()` does not spawn extra workers.
- [ ] Expand worker-native smoke to more compiled packages (as available in system library).
- [ ] Keep serial benchmark parity at each checkpoint before increasing worker coverage.

## Notes / Questions to Keep In Mind
- Serial parity is non-negotiable: any added checks/atomics/dispatch on hot serial paths must be avoided.
- Symbol resolution should be fast in all threads:
  - If the symbol already exists in the symbol table, resolving it should not require a global lock (or should be extremely lightweight).
- R does not have true refcounting (only a few bits in the header), so cross-heap ownership/transfer needs an approach that does not depend on unbounded refcounts.
- `.Call` / `.External` compatibility is part of baseline package usability; package authors should not need a new registration model.
- Loading packages can stay main-thread-only initially, but calling already-registered routines must remain transparent.
- Current safety tradeoff in `mtlapply` error handling:
  - For `threads > 1`, the main thread is currently coordinator-only (workers evaluate `FUN`; main services RPC/waits).
  - This avoids concurrent main+worker error-unwind corruption seen in mixed-error workloads.
  - Effective parallel workers are `threads - 1` for now; revisit once cross-thread error machinery is hardened.
- Keep this regression in the default loop:
  - `mtlapply(..., threads > 1)` with a deliberate `stop()` must return a recoverable error.
  - A subsequent regular `lapply()` call and regular main-thread error handling must still work.
