
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

# R-devel (no mtlapply)
/usr/local/bin/R-devel --vanilla -q -f bench/readme_bench_run.R --args bench/results/rdevel.rds

# experimental build (has mtlapply)
./build-mtl/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl.rds

# optional: generate a bench::mark artifact under the experimental build
R_LIBS_USER=/private/tmp/mtl-proof-lib-MW1vv9 R_LIBS_SITE='' \
  ./build-mtl/bin/R --vanilla -q -f bench/readme_bench_mark.R --args bench/results/mtl_bench_mark.rds
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
| alloc_pressure | mtl | lapply | 0.111 | 0.919 | 0.856 | 1.000 | NA |
| alloc_pressure | mtl | mtlapply(1) | 0.113 | 0.903 | 0.841 | 0.982 | 0.982 |
| alloc_pressure | mtl | mtlapply(2) | 0.077 | 1.325 | 1.234 | 1.442 | 0.721 |
| alloc_pressure | mtl | mtlapply(4) | 0.043 | 2.372 | 2.209 | 2.581 | 0.645 |
| alloc_pressure | mtl | mtlapply(8) | 0.027 | 3.778 | 3.519 | 4.111 | 0.514 |
| alloc_pressure | rdevel | lapply | 0.102 | 1.000 | 0.931 | 1.088 | NA |
| alloc_pressure | system | lapply | 0.095 | 1.074 | 1.000 | 1.168 | NA |
| cos_seq | mtl | lapply | 0.420 | 1.048 | 0.950 | 1.000 | NA |
| cos_seq | mtl | mtlapply(1) | 0.435 | 1.011 | 0.917 | 0.966 | 0.966 |
| cos_seq | mtl | mtlapply(2) | 0.255 | 1.725 | 1.565 | 1.647 | 0.824 |
| cos_seq | mtl | mtlapply(4) | 0.147 | 2.993 | 2.714 | 2.857 | 0.714 |
| cos_seq | mtl | mtlapply(8) | 0.081 | 5.432 | 4.926 | 5.185 | 0.648 |
| cos_seq | rdevel | lapply | 0.440 | 1.000 | 0.907 | 0.955 | NA |
| cos_seq | system | lapply | 0.399 | 1.103 | 1.000 | 1.053 | NA |
| etl_group_mean | mtl | lapply | 0.922 | 1.065 | 0.905 | 1.000 | NA |
| etl_group_mean | mtl | mtlapply(1) | 0.918 | 1.070 | 0.908 | 1.004 | 1.004 |
| etl_group_mean | mtl | mtlapply(2) | 0.486 | 2.021 | 1.716 | 1.897 | 0.949 |
| etl_group_mean | mtl | mtlapply(4) | 0.254 | 3.866 | 3.283 | 3.630 | 0.907 |
| etl_group_mean | mtl | mtlapply(8) | 0.126 | 7.794 | 6.619 | 7.317 | 0.915 |
| etl_group_mean | rdevel | lapply | 0.982 | 1.000 | 0.849 | 0.939 | NA |
| etl_group_mean | system | lapply | 0.834 | 1.177 | 1.000 | 1.106 | NA |

    ## 
    ## Single-thread overhead check (per workload):

| workload       | ratio_mtl_vs_sys | ratio_mtl_vs_rdevel |
|:---------------|-----------------:|--------------------:|
| alloc_pressure |            1.168 |               1.088 |
| cos_seq        |            1.053 |               0.955 |
| etl_group_mean |            1.106 |               0.939 |

    ## 
    ## Scaling (speedup vs `mtl` lapply):

![](bench/figures/readme-results-1.png)<!-- -->

    ## 
    ## bench::mark (mtl build, same workload; plotted):

    ## # A data frame: 5 × 13
    ##   expression    min median `itr/sec` mem_alloc `gc/sec` n_itr  n_gc total_time
    ##   <bch:expr>  <dbl>  <dbl>     <dbl> <bch:byt>    <dbl> <int> <dbl>      <dbl>
    ## 1 lapply      0.909  0.935      1.07        NA     57.7     3   161      2.79 
    ## 2 mtlapply(1) 0.914  0.923      1.08        NA     42.2     3   117      2.77 
    ## 3 mtlapply(2) 0.480  0.481      2.07        NA     40.1     3    58      1.45 
    ## 4 mtlapply(4) 0.253  0.254      3.93        NA     44.5     3    34      0.763
    ## 5 mtlapply(8) 0.128  0.128      7.77        NA     36.2     3    14      0.386
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
