
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
- `./build-mtl-shlib/bin/R` from this tree (to measure `mtlapply()` scaling)

For binary-package ABI compatibility on macOS, use the `--enable-R-shlib` build
(`build-mtl-shlib`). Non-shlib executables can load a second `libR.dylib` when
loading prebuilt package binaries.

Generate the timing artifacts:

``` sh
mkdir -p bench/results

# system R (no mtlapply)
R --vanilla -q -f bench/readme_bench_run.R --args bench/results/system.rds

# R-devel (no mtlapply)
/usr/local/bin/R-devel --vanilla -q -f bench/readme_bench_run.R --args bench/results/rdevel.rds

# experimental build (has mtlapply)
./build-mtl-shlib/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl.rds

# optional: generate a bench::mark artifact under the experimental build
R_LIBS_USER=/private/tmp/mtl-proof-lib-MW1vv9 R_LIBS_SITE='' \
  ./build-mtl-shlib/bin/R --vanilla -q -f bench/readme_bench_mark.R --args bench/results/mtl_bench_mark.rds
```

Then render this README, which loads those artifacts and summarizes
them:

    ## Settings:

| setting    | value   |
|:-----------|:--------|
| N          | 2000000 |
| nshards    | 64      |
| ngroups    | 4096    |
| feat_loops | 40      |
| cos_m      | 200000  |
| cos_k      | 256     |
| alloc_m    | 50000   |
| alloc_k    | 128     |
| iters      | 3       |
| threads    | 1,2,4,8 |

    ## 
    ## R versions:

| build  | r_version                                          |
|:-------|:---------------------------------------------------|
| system | R version 4.5.2 (2025-10-31)                       |
| rdevel | R Under development (unstable) (2026-02-09 r89390) |
| mtl    | R Under development (unstable) (2026-02-10 r99999) |

    ## 
    ## Timings:

| workload | build | label | median_seconds | speedup_vs_rdevel_lapply | speedup_vs_sys_lapply | speedup_vs_mtl_lapply | efficiency_vs_mtl_lapply |
|:---|:---|:---|---:|---:|---:|---:|---:|
| alloc_pressure | mtl | lapply | 0.122 | 0.910 | 0.959 | 1.000 | NA |
| alloc_pressure | mtl | mtlapply(1) | 0.115 | 0.965 | 1.017 | 1.061 | 1.061 |
| alloc_pressure | mtl | mtlapply(2) | 0.130 | 0.854 | 0.900 | 0.938 | 0.469 |
| alloc_pressure | mtl | mtlapply(4) | 0.072 | 1.542 | 1.625 | 1.694 | 0.424 |
| alloc_pressure | mtl | mtlapply(8) | 0.045 | 2.467 | 2.600 | 2.711 | 0.339 |
| alloc_pressure | rdevel | lapply | 0.111 | 1.000 | 1.054 | 1.099 | NA |
| alloc_pressure | system | lapply | 0.117 | 0.949 | 1.000 | 1.043 | NA |
| cos_seq | mtl | lapply | 0.485 | 0.969 | 0.963 | 1.000 | NA |
| cos_seq | mtl | mtlapply(1) | 0.460 | 1.022 | 1.015 | 1.054 | 1.054 |
| cos_seq | mtl | mtlapply(2) | 0.339 | 1.386 | 1.378 | 1.431 | 0.715 |
| cos_seq | mtl | mtlapply(4) | 0.184 | 2.554 | 2.538 | 2.636 | 0.659 |
| cos_seq | mtl | mtlapply(8) | 0.105 | 4.476 | 4.448 | 4.619 | 0.577 |
| cos_seq | rdevel | lapply | 0.470 | 1.000 | 0.994 | 1.032 | NA |
| cos_seq | system | lapply | 0.467 | 1.006 | 1.000 | 1.039 | NA |
| etl_group_mean | mtl | lapply | 0.999 | 1.085 | 0.941 | 1.000 | NA |
| etl_group_mean | mtl | mtlapply(1) | 0.983 | 1.103 | 0.956 | 1.016 | 1.016 |
| etl_group_mean | mtl | mtlapply(2) | 0.619 | 1.751 | 1.519 | 1.614 | 0.807 |
| etl_group_mean | mtl | mtlapply(4) | 0.309 | 3.508 | 3.042 | 3.233 | 0.808 |
| etl_group_mean | mtl | mtlapply(8) | 0.156 | 6.949 | 6.026 | 6.404 | 0.800 |
| etl_group_mean | rdevel | lapply | 1.084 | 1.000 | 0.867 | 0.922 | NA |
| etl_group_mean | system | lapply | 0.940 | 1.153 | 1.000 | 1.063 | NA |

    ## 
    ## Single-thread overhead check (per workload):

| workload       | ratio_mtl_vs_sys | ratio_mtl_vs_rdevel |
|:---------------|-----------------:|--------------------:|
| alloc_pressure |            1.043 |               1.099 |
| cos_seq        |            1.039 |               1.032 |
| etl_group_mean |            1.063 |               0.922 |

    ## 
    ## Scaling (speedup vs `mtl` lapply):

![](bench/figures/readme-results-1.png)<!-- -->

    ## 
    ## bench::mark (mtl build, same workload; plotted):

    ## # A data frame: 5 × 13
    ##   expression    min median `itr/sec` mem_alloc `gc/sec` n_itr  n_gc total_time
    ##   <bch:expr>  <dbl>  <dbl>     <dbl> <bch:byt>    <dbl> <int> <dbl>      <dbl>
    ## 1 lapply      0.950  0.984      1.03        NA     58.2     3   170      2.92 
    ## 2 mtlapply(1) 0.986  0.987      1.01        NA     38.7     3   115      2.97 
    ## 3 mtlapply(2) 0.609  0.611      1.64        NA     30.6     3    56      1.83 
    ## 4 mtlapply(4) 0.311  0.311      3.21        NA     33.2     3    31      0.934
    ## 5 mtlapply(8) 0.153  0.158      6.39        NA     17.0     3     8      0.469
    ## # ℹ 4 more variables: result <list>, memory <list>, time <list>, gc <list>

![](bench/figures/readme-results-2.png)<!-- -->

## Notes / Limitations

- Workloads that hammer global mutable runtime structures (notably
  string/symbol interning) may scale poorly.
- Some operations are still serialized under a global lock when run from
  workers (notably selected native interfaces), to avoid corrupting
  global process state.
- This is runtime work: correctness and safety come before “make
  everything parallel”.
