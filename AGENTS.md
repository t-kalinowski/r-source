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

### Benchmarks and Regression Checks
- Compare against the system development build at `/usr/local/bin/R-devel` for apples-to-apples timing.
- Run heavier benchmarks (larger workloads, more iterations) to reduce measurement noise.
- Persist benchmark results as artifacts (e.g. `.rds`) and load/compare them later.

### Process Management During Benchmarking
- Prefer running long or kill-prone commands via the MCP console:
  - Spawn work with `system2()` inside the console session.
  - If something wedges or spawns children, use `manage_session(\"restart\")` to cleanly kill the session and its children, then continue.

## Notes / Questions to Keep In Mind
- Serial parity is non-negotiable: any added checks/atomics/dispatch on hot serial paths must be avoided.
- Symbol resolution should be fast in all threads:
  - If the symbol already exists in the symbol table, resolving it should not require a global lock (or should be extremely lightweight).
- R does not have true refcounting (only a few bits in the header), so cross-heap ownership/transfer needs an approach that does not depend on unbounded refcounts.

