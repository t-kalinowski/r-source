
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
    ## R versions:

    ## - system: R version 4.5.2 (2025-10-31)

    ## - mtl:    R version 4.6.0 Under development (unstable) (1970-01-01)

    ## 
    ## Timings:

| build | label | median_seconds | speedup_vs_sys_lapply | speedup_vs_mtl_lapply |
|:---|:---|---:|---:|---:|
| mtl | lapply | 0.999 | 0.958 | 1.000 |
| mtl | mtlapply(1) | 0.945 | 1.013 | 1.057 |
| mtl | mtlapply(2) | 0.475 | 2.015 | 2.103 |
| mtl | mtlapply(4) | 0.239 | 4.004 | 4.180 |
| mtl | mtlapply(8) | 0.120 | 7.975 | 8.325 |
| system | lapply | 0.957 | 1.000 | 1.044 |

    ## 
    ## Single-thread overhead check:

    ## mtl lapply / system lapply = 1.044x

## Notes / Limitations

- Workloads that hammer global mutable runtime structures (notably
  string/symbol interning) may scale poorly.
- Some operations are still serialized under a global lock when run from
  workers (notably selected native interfaces), to avoid corrupting
  global process state.
- This is runtime work: correctness and safety come before “make
  everything parallel”.
