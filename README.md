
# Threaded R Runtime (Experimental)

This branch experiments with evaluating R code on multiple OS threads
inside one process.

Current user-facing APIs:

- `mtlapply(X, FUN, ...)`: threaded apply with the same signature as
  `lapply()`.
- `background(expr, env = parent.frame())`: enqueue work on the shared
  thread pool.
- `wait(futures, timeout = Inf)`: pop the next completed future.
- `cancel(future)`: request cancellation for pending/running futures.

Thread-pool size is controlled globally with:

``` r
options(mtlapply.threads = 8L)
```

## Quick Start

``` r
options(mtlapply.threads = 8L)

# 1) mtlapply
A <- matrix(runif(1000 * 1000), 1000, 1000)
B <- matrix(runif(1000 * 1000), 1000, 1000)
system.time(lapply(1:20, \(i) (A %*% B) + i))
system.time(mtlapply(1:20, \(i) (A %*% B) + i))

# 2) background + wait
work <- function(i) {
  x <- as.double(i)
  for (k in seq_len(500000L)) {
    x <- x + sin(k + x) + cos(k - x * 0.5)
  }
  x
}

pending <- lapply(1:32, \(i) background(work(i)))
out <- vector("list", length(pending))
ids <- seq_along(pending)

while (length(pending)) {
  got <- wait(pending)
  idx <- as.integer(attr(got, "index")[[1L]])
  out[[ids[[idx]]]] <- got$value
  pending <- pending[-idx]
  ids <- ids[-idx]
}
```

## Benchmark Snapshot (Committed Artifacts)

The tables below are generated from committed artifacts in
`bench/results/`. Regenerate them with `tools/mtl-bench-refresh.sh`,
then inspect changes with `git diff`.

## 1) Serial Parity: `lapply` vs `R-devel`

Goal: no single-thread slowdown for normal serial code.

| workload       | rdevel_lapply_s | mtl_lapply_s | ratio_mtl_vs_rdevel |
|:---------------|----------------:|-------------:|--------------------:|
| alloc_pressure |           0.109 |        0.101 |               0.927 |
| cos_seq        |           0.495 |        0.408 |               0.824 |
| etl_group_mean |           1.043 |        0.979 |               0.939 |

Interpretation:

- `ratio_mtl_vs_rdevel <= 1` means parity or better.
- In this checkpoint artifact, all listed workloads are at or faster
  than `R-devel` for `lapply`.

## 2) `mtlapply` Scaling and Efficiency

Speedup baseline here is `mtl` build `lapply` on the same workload.
Efficiency is `speedup / threads`.

| workload       | threads | mtlapply_s | lapply_s | speedup | efficiency |
|:---------------|--------:|-----------:|---------:|--------:|-----------:|
| alloc_pressure |       1 |      0.095 |    0.101 |   1.063 |      1.063 |
| alloc_pressure |       2 |      0.114 |    0.101 |   0.886 |      0.443 |
| alloc_pressure |       4 |      0.065 |    0.101 |   1.554 |      0.388 |
| alloc_pressure |       8 |      0.108 |    0.101 |   0.935 |      0.117 |
| cos_seq        |       1 |      0.413 |    0.408 |   0.988 |      0.988 |
| cos_seq        |       2 |      0.301 |    0.408 |   1.355 |      0.678 |
| cos_seq        |       4 |      0.177 |    0.408 |   2.305 |      0.576 |
| cos_seq        |       8 |      0.181 |    0.408 |   2.254 |      0.282 |
| etl_group_mean |       1 |      0.968 |    0.979 |   1.011 |      1.011 |
| etl_group_mean |       2 |      0.601 |    0.979 |   1.629 |      0.814 |
| etl_group_mean |       4 |      0.302 |    0.979 |   3.242 |      0.810 |
| etl_group_mean |       8 |      0.152 |    0.979 |   6.441 |      0.805 |

![](README_files/figure-gfm/mtlapply-speedup-plot-1.png)<!-- -->

Workload behavior from this run:

- `etl_group_mean`: near-linear, strong scaling to 8 threads.
- `cos_seq`: scales through 4 threads, then plateaus at 8 threads.
- `alloc_pressure`: weak scaling; expected for allocation/GC-heavy
  closures.

## 3) Dense Numeric Workload (Matrix Multiply Loop)

This comes from `tools/mtl-threadpool-perf-smoke.R` with 8 threads.

| case | rows | cols | n | threads | reps | lapply_median_s | mtlapply_median_s | speedup | efficiency |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| matmul_1000_n20 | 1000 | 1000 | 20 | 8 | 3 | 5.277 | 0.804 | 6.563 | 0.820 |
| matmul_100_n20000 | 100 | 100 | 20000 | 8 | 3 | 6.388 | 0.813 | 7.857 | 0.982 |

At this checkpoint:

- `matmul_1000_n20`: 6.52x speedup, 81.5% efficiency.
- `matmul_100_n20000`: 7.87x speedup, 98.4% efficiency.

## 4) `background()` / `wait()` Burst Throughput

This is a Shiny-style request burst simulation from
`tools/mtl-shiny-background-smoke.R`.

| mode | threads | elapsed_s | throughput_req_s | p50_s | p95_s | p99_s | speedup_vs_serial | p95_gain_vs_serial |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| background | 2 | 0.273 | 351.648 | 0.148 | 0.270 | 0.272 | 2.267 | 2.175 |
| background | 4 | 0.274 | 350.365 | 0.148 | 0.273 | 0.274 | 2.259 | 2.153 |
| background | 8 | 0.273 | 351.648 | 0.148 | 0.271 | 0.272 | 2.267 | 2.169 |
| serial | 1 | 0.619 | 155.089 | 0.313 | 0.588 | 0.612 | 1.000 | 1.000 |

For the 8-thread row in this checkpoint:

- Throughput speedup vs serial: ~2.22x.
- P95 gain vs serial: ~2.12x.
- Efficiency (speedup / 8): ~27.8%.

This reflects queueing/coordination overhead and main-thread
orchestration in the current `background()/wait()` path.

## 5) End-to-End Shiny Load Test (Full Recording, No Failures)

From `tools/shiny-threadpool-bench`:

- sync app: `label_sync`
- threadpool app: `label_mt` (`ExtendedTask(offload = "threadpool")`)
- `workers = 8`
- `SHINY_BENCH_WORK_SCALE = 300`
- full recording replay (`recording.log`)

| metric                          |    sync | threadpool |    gain |
|:--------------------------------|--------:|-----------:|--------:|
| median_total_s                  | 175.144 |     23.140 |   7.569 |
| median_busy_s                   | 153.024 |      1.024 | 149.365 |
| p95_busy_s                      | 153.366 |      1.137 | 134.893 |
| throughput_completed_sess_per_s |   0.046 |      0.332 |   7.289 |

For this run, both modes completed with zero failures
(`sessions_failed = 0`), and the threadpool app improved both latency
and throughput materially.

## Reproducing These Benchmarks

``` sh
# One-shot refresh of committed benchmark artifacts + markdown outputs
tools/mtl-bench-refresh.sh build-mtl-shlib R /usr/local/bin/R-devel

# Then inspect impact
git diff -- bench/LATEST.md README.md bench/results/
```

## Guardrails and Unsupported Patterns (Current)

`mtlapply()` workers are read-mostly with respect to process-global
state.

- Writes to `globalenv()` from worker threads are rejected.
- Worker writes to `options(mtlapply.threads = ...)` or
  `options(threads = ...)` are rejected.
- Worker `options()` changes are job-local and do not propagate to the
  main thread.
- Some operations with shared process-wide state may serialize under an
  internal global lock.
- Worker registration of R-level finalizer functions is not supported.
- JIT compilation of closures is currently disabled in worker threads.

In practice: pure/mostly-local closure workloads scale well;
global-state and allocation-heavy workloads can flatten or regress.
