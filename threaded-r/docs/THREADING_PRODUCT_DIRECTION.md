# Threaded R Product Direction (Distilled)

## 1. Why this exists

This effort aims to make a threaded build of R viable as a daily-driver, drop-in replacement:

- existing serial workflows should remain fast,
- existing package ecosystems should continue to work,
- safe workloads should gain substantial throughput/latency wins.

The objective is not a one-off prototype; it is a maintainable architecture that can evolve with upstream R.

## 2. Core design goals

1. Serial parity first
- When no threaded work is active, main-thread execution should be near-upstream performance.
- Any serial slowdown is treated as a regression unless explicitly justified.

2. True parallel worker execution
- Support concurrent evaluation of ordinary R closures, not only native kernels.
- Use a persistent threadpool; avoid per-call worker bring-up/teardown.

3. Subinterpreter isolation
- Each worker has independent interpreter runtime state, including:
  - protect stack,
  - evaluation context state,
  - heap/allocator domain.

4. Safe result return
- Worker results are returned to caller with explicit ownership transfer/splicing semantics.
- Avoid expensive serialization as the default path.

5. Package compatibility
- Existing CRAN package source and binary artifacts should work without code changes.
- Already-loaded native routines should be callable from worker threads.

6. Recoverable errors
- Worker errors should propagate as normal R errors with clear tracebacks.
- After a worker error, subsequent ordinary R evaluation must remain healthy.

## 3. Runtime invariants

### 3.1 Global-state rules

- Worker reads from global/search-path state are allowed.
- Worker writes to process-global state are not allowed by default.
- Forbidden worker-side actions should fail with deterministic, user-facing errors.

Examples:

- `setwd()` in worker: error.
- assignment into `globalenv()` in worker: error.
- closure-local mutations in worker: allowed.

### 3.2 Threadpool model

- Pool is persistent and shared by all threaded APIs.
- `options(threads = n)` controls active worker concurrency.
- Nested threaded calls reuse the same pool.
- Queue policy should prevent nested deadlocks and minimize idle starvation.

### 3.3 C API behavior

- `.Call` / `.External` support is required.
- Native routines that only inspect/compute/allocate local outputs should run concurrently.
- Operations that mutate process-global runtime tables may be serialized via main-thread RPC/locks.
- Package loading/registration may be main-thread constrained initially.

## 4. User-facing API shape (initial)

### `mtlapply`

```r
options(threads = 8L)
out <- mtlapply(X, FUN, ...)
```

- Signature intentionally mirrors `lapply`.
- No `threads` argument on function call.

### futures/threadpool

```r
f1 <- background(expr1)
f2 <- background(expr2)
ready <- wait(list(f1, f2))
val <- ready$value
```

- `background()`: enqueue closure/expression onto pool; return future handle.
- `wait()`: wait for next completed future among a set (pop-done behavior).
- `cancel()`: best-effort cancel if work has not started.

## 5. Performance doctrine

1. Protect serial path
- Keep idle-threadpool fast path extremely close to upstream evaluator/allocator paths.
- Avoid adding synchronization, branch, or indirection overhead to hot serial loops.

2. Scale where safe
- Compute-heavy and mixed workloads should show near-linear gains to moderate core counts.
- IO-overlap workflows should benefit from concurrent worker execution.

3. Measure continuously
- Every checkpoint carries benchmark artifacts and a rendered dashboard.
- Regressions are caught by CI/local gates, not discovered late.

## 6. Compatibility doctrine

- ABI compatibility with already-built packages is a release blocker.
- Runtime/linking smoke tests (including embedded `rsession`) are part of default validation.
- Any change that improves microbench numbers but regresses package compatibility is rejected.

## 7. Error handling doctrine

- First worker failure should trigger deterministic job cancellation/quiescence.
- Main thread should return promptly with a clean, recoverable error.
- Teardown may complete asynchronously, but state visible to user must be consistent.

## 8. What this document excludes

- It does not prescribe every data structure.
- It does not freeze queue internals.
- It does define non-negotiable behavior and acceptance criteria for architecture choices.
