
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

## Benchmark Snapshot (Checkpoint 2026-02-12)

The tables below are from concrete benchmark runs in this branch at the
checkpoint tagged on 2026-02-12.

## 1) Serial Parity: `lapply` vs `R-devel`

Goal: no single-thread slowdown for normal serial code.

| workload       | rdevel_lapply_s | mtl_lapply_s | ratio_mtl_vs_rdevel |
|:---------------|----------------:|-------------:|--------------------:|
| etl_group_mean |           1.095 |        0.977 |               0.892 |
| cos_seq        |           0.504 |        0.426 |               0.845 |
| alloc_pressure |           0.121 |        0.101 |               0.835 |

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
| alloc_pressure |       4 |      0.066 |    0.101 |   1.530 |      0.383 |
| alloc_pressure |       8 |      0.102 |    0.101 |   0.990 |      0.124 |
| cos_seq        |       1 |      0.410 |    0.426 |   1.039 |      1.039 |
| cos_seq        |       2 |      0.293 |    0.426 |   1.454 |      0.727 |
| cos_seq        |       4 |      0.167 |    0.426 |   2.551 |      0.638 |
| cos_seq        |       8 |      0.167 |    0.426 |   2.551 |      0.319 |
| etl_group_mean |       1 |      0.933 |    0.977 |   1.047 |      1.047 |
| etl_group_mean |       2 |      0.593 |    0.977 |   1.648 |      0.824 |
| etl_group_mean |       4 |      0.304 |    0.977 |   3.214 |      0.803 |
| etl_group_mean |       8 |      0.153 |    0.977 |   6.386 |      0.798 |

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
| matmul_1000_n20 | 1000 | 1000 | 20 | 8 | 3 | 5.249 | 0.805 | 6.520 | 0.815 |
| matmul_100_n20000 | 100 | 100 | 20000 | 8 | 3 | 6.386 | 0.811 | 7.874 | 0.984 |

At this checkpoint:

- `matmul_1000_n20`: 6.52x speedup, 81.5% efficiency.
- `matmul_100_n20000`: 7.87x speedup, 98.4% efficiency.

## 4) `background()` / `wait()` Burst Throughput

This is a Shiny-style request burst simulation from
`tools/mtl-shiny-background-smoke.R`.

| mode | threads | elapsed_s | throughput_req_s | p50_s | p95_s | p99_s | speedup_vs_serial | p95_gain_vs_serial |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| background | 2 | 0.280 | 342.857 | 0.152 | 0.277 | 0.280 | 2.214 | 2.126 |
| background | 4 | 0.280 | 342.857 | 0.152 | 0.277 | 0.280 | 2.214 | 2.126 |
| background | 8 | 0.279 | 344.086 | 0.152 | 0.278 | 0.278 | 2.222 | 2.121 |
| serial | 1 | 0.620 | 154.839 | 0.313 | 0.590 | 0.613 | 1.000 | 1.000 |

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

| mode | sessions_total | sessions_completed | sessions_failed | median_total_s | median_busy_s | p95_busy_s | throughput_completed_sess_per_s |
|:---|---:|---:|---:|---:|---:|---:|---:|
| sync | 8 | 8 | 0 | 175.144 | 153.024 | 153.366 | 0.046 |
| threadpool | 8 | 8 | 0 | 23.140 | 1.024 | 1.137 | 0.332 |

| metric                          |    sync | threadpool |    gain |
|:--------------------------------|--------:|-----------:|--------:|
| median_total_s                  | 175.144 |     23.140 |   7.569 |
| median_busy_s                   | 153.024 |      1.024 | 149.438 |
| p95_busy_s                      | 153.366 |      1.137 | 134.887 |
| throughput_completed_sess_per_s |   0.046 |      0.332 |   7.217 |

For this run, both modes completed with zero failures
(`sessions_failed = 0`), and the threadpool app improved both latency
and throughput materially.

## Reproducing These Benchmarks

``` sh
# Serial parity + mtlapply scaling artifacts
R --vanilla -q -f bench/readme_bench_run.R --args bench/results/system_latest.rds
/usr/local/bin/R-devel --vanilla -q -f bench/readme_bench_run.R --args bench/results/rdevel_latest.rds
build-mtl-shlib/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl_latest.rds

# Matrix benchmark
build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-threadpool-perf-smoke.R --args 8 3 0.45

# background()/wait() burst benchmark
build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-shiny-background-smoke.R --args \
  bench/results/mtl_shiny_background_smoke_checkpoint.csv \
  bench/figures/mtl_shiny_background_smoke_checkpoint.png \
  96 3 2,4,8 1.5 120000
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
