
# R Multi-Threaded Interpreter Experiment (`mtlapply`)

This repo is an experimental R runtime that can evaluate *pure R*
closures concurrently on multiple OS threads, in a single R process,
using `mtlapply()` (modeled after `lapply()`/`mclapply()`).

The hook is simple: when your workload is “a lot of independent R work”
(feature engineering, per-shard transforms, per-group summaries),
`mtlapply()` can give near-linear speedups without forking and without
serializing return values.

## A Benchmark You Can Read (And Reproduce)

The “unit of parallelism” here is a shard id. There is no up-front
“pre-splitting”; the `mtlapply()` call is what shards the work.

``` r
# This is the actual benchmark code (identical in shape to bench/readme_bench_run.R).
# The key idea: mtlapply() shards work by shard id; the worker computes its own
# row indices deterministically. No up-front "split" required.

N <- 2e6L
nshards <- 64L
ngroups <- 4096L
feat_loops <- 40L

# Deterministic data (no RNG, no strings).
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)

worker <- function(shard_id) {
  idx <- seq.int(shard_id, N, by = nshards)
  z <- x[idx]
  for (i in seq_len(feat_loops)) {
    z <- log1p(abs(z)) + sin(y[idx] + z) * w[idx] + cos(z - y[idx])
  }
  g <- grp[idx]  # groups for summarise
  # summarise: group-wise sum, plus counts
  n <- tabulate(g, ngroups)
  s <- numeric(ngroups)
  for (j in seq_along(z)) {
    gj <- g[[j]]
    s[[gj]] <- s[[gj]] + z[[j]]
  }
  list(sum = s, n = n)
}

reduce <- function(parts) {
  s <- Reduce(`+`, lapply(parts, `[[`, "sum"))
  n <- Reduce(`+`, lapply(parts, `[[`, "n"))
  s / n
}

ids <- seq_len(nshards)

# baseline
system.time(reduce(lapply(ids, worker)))[["elapsed"]]

# parallel (in the experimental build)
system.time(reduce(mtlapply(ids, worker, threads = 8L)))[["elapsed"]]
```

## Real Numbers (Loaded From Artifacts)

The benchmark is run as a standalone base-R script under:

- the system `R` (to check for single-threaded regressions)
- `./build-mtl/bin/R` from this tree (to measure `mtlapply()` scaling)

Generate the timing artifacts:

``` sh
mkdir -p bench/results

# system R (no mtlapply)
R --vanilla -q -f bench/readme_bench_run.R --args bench/results/system.rds

# experimental build (has mtlapply)
R_HOME= ./build-mtl/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl.rds
```

Then render this README, which loads those artifacts and summarizes
them:

    ## Settings:

    ## $N
    ## [1] 2000000
    ## 
    ## $nshards
    ## [1] 64
    ## 
    ## $ngroups
    ## [1] 4096
    ## 
    ## $feat_loops
    ## [1] 40
    ## 
    ## $iters
    ## [1] 3
    ## 
    ## $threads
    ## [1] 1 2 4 8

    ## 
    ## Workloads:

    ## $etl_group_mean
    ## list()
    ## 
    ## $cos_seq
    ## $cos_seq$m
    ## [1] 200000
    ## 
    ## $cos_seq$k
    ## [1] 256
    ## 
    ## 
    ## $alloc_pressure
    ## $alloc_pressure$m
    ## [1] 50000
    ## 
    ## $alloc_pressure$k
    ## [1] 128

    ## 
    ## R versions:

    ## - system: R version 4.5.2 (2025-10-31)

    ## - mtl:    R version 4.6.0 Under development (unstable) (1970-01-01)

    ## 
    ## Timings:

| workload | build | label | median_seconds | speedup_vs_sys_lapply | speedup_vs_mtl_lapply | efficiency_vs_mtl_lapply |
|:---|:---|:---|---:|---:|---:|---:|
| alloc_pressure | mtl | lapply | 0.156 | 0.686 | 1.000 | NA |
| alloc_pressure | mtl | mtlapply(1) | 0.114 | 0.939 | 1.368 | 1.368 |
| alloc_pressure | mtl | mtlapply(2) | 0.066 | 1.621 | 2.364 | 1.182 |
| alloc_pressure | mtl | mtlapply(4) | 0.037 | 2.892 | 4.216 | 1.054 |
| alloc_pressure | mtl | mtlapply(8) | 0.027 | 3.963 | 5.778 | 0.722 |
| alloc_pressure | system | lapply | 0.107 | 1.000 | 1.458 | NA |
| cos_seq | mtl | lapply | 0.540 | 0.848 | 1.000 | NA |
| cos_seq | mtl | mtlapply(1) | 0.418 | 1.096 | 1.292 | 1.292 |
| cos_seq | mtl | mtlapply(2) | 0.235 | 1.949 | 2.298 | 1.149 |
| cos_seq | mtl | mtlapply(4) | 0.136 | 3.368 | 3.971 | 0.993 |
| cos_seq | mtl | mtlapply(8) | 0.076 | 6.026 | 7.105 | 0.888 |
| cos_seq | system | lapply | 0.458 | 1.000 | 1.179 | NA |
| etl_group_mean | mtl | lapply | 0.977 | 0.940 | 1.000 | NA |
| etl_group_mean | mtl | mtlapply(1) | 0.946 | 0.970 | 1.033 | 1.033 |
| etl_group_mean | mtl | mtlapply(2) | 0.473 | 1.941 | 2.066 | 1.033 |
| etl_group_mean | mtl | mtlapply(4) | 0.238 | 3.857 | 4.105 | 1.026 |
| etl_group_mean | mtl | mtlapply(8) | 0.119 | 7.714 | 8.210 | 1.026 |
| etl_group_mean | system | lapply | 0.918 | 1.000 | 1.064 | NA |

    ## 
    ## Single-thread overhead check (per workload):

| workload       | ratio |
|:---------------|------:|
| alloc_pressure | 1.458 |
| cos_seq        | 1.179 |
| etl_group_mean | 1.064 |

## Notes / Limitations

- Workloads that hammer global mutable runtime structures (notably
  string/symbol interning) may scale poorly.
- Some operations are still serialized under a global lock when run from
  workers (notably selected native interfaces), to avoid corrupting
  global process state.
- This is runtime work: correctness and safety come before “make
  everything parallel”.
