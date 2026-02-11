
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
| cos_m      | 200000  |
| cos_k      | 256     |
| alloc_m    | 50000   |
| alloc_k    | 128     |
| iters      | 5       |
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
| alloc_pressure | mtl | lapply | 0.108 | 0.972 | 0.972 | 1.000 | NA |
| alloc_pressure | mtl | mtlapply(1) | 0.110 | 0.955 | 0.955 | 0.982 | 0.982 |
| alloc_pressure | mtl | mtlapply(2) | 0.124 | 0.847 | 0.847 | 0.871 | 0.435 |
| alloc_pressure | mtl | mtlapply(4) | 0.067 | 1.567 | 1.567 | 1.612 | 0.403 |
| alloc_pressure | mtl | mtlapply(8) | 0.044 | 2.386 | 2.386 | 2.455 | 0.307 |
| alloc_pressure | rdevel | lapply | 0.105 | 1.000 | 1.000 | 1.029 | NA |
| alloc_pressure | system | lapply | 0.105 | 1.000 | 1.000 | 1.029 | NA |
| cos_seq | mtl | lapply | 0.426 | 1.089 | 1.026 | 1.000 | NA |
| cos_seq | mtl | mtlapply(1) | 0.436 | 1.064 | 1.002 | 0.977 | 0.977 |
| cos_seq | mtl | mtlapply(2) | 0.316 | 1.468 | 1.383 | 1.348 | 0.674 |
| cos_seq | mtl | mtlapply(4) | 0.175 | 2.651 | 2.497 | 2.434 | 0.609 |
| cos_seq | mtl | mtlapply(8) | 0.103 | 4.505 | 4.243 | 4.136 | 0.517 |
| cos_seq | rdevel | lapply | 0.464 | 1.000 | 0.942 | 0.918 | NA |
| cos_seq | system | lapply | 0.437 | 1.062 | 1.000 | 0.975 | NA |
| etl_group_mean | mtl | lapply | 0.928 | 1.111 | 0.958 | 1.000 | NA |
| etl_group_mean | mtl | mtlapply(1) | 0.929 | 1.110 | 0.957 | 0.999 | 0.999 |
| etl_group_mean | mtl | mtlapply(2) | 0.598 | 1.724 | 1.487 | 1.552 | 0.776 |
| etl_group_mean | mtl | mtlapply(4) | 0.307 | 3.358 | 2.896 | 3.023 | 0.756 |
| etl_group_mean | mtl | mtlapply(8) | 0.154 | 6.695 | 5.773 | 6.026 | 0.753 |
| etl_group_mean | rdevel | lapply | 1.031 | 1.000 | 0.862 | 0.900 | NA |
| etl_group_mean | system | lapply | 0.889 | 1.160 | 1.000 | 1.044 | NA |

    ## 
    ## Single-thread overhead check (per workload):

| workload       | ratio_mtl_vs_sys | ratio_mtl_vs_rdevel |
|:---------------|-----------------:|--------------------:|
| alloc_pressure |            1.029 |               1.029 |
| cos_seq        |            0.975 |               0.918 |
| etl_group_mean |            1.044 |               0.900 |

    ## 
    ## Scaling (speedup vs `mtl` lapply):

![](bench/figures/readme-results-1.png)<!-- -->

    ## 
    ## bench::mark (mtl build, same workload; plotted):

    ## # A data frame: 5 × 13
    ##   expression    min median `itr/sec` mem_alloc `gc/sec` n_itr  n_gc total_time
    ##   <bch:expr>  <dbl>  <dbl>     <dbl> <bch:byt>    <dbl> <int> <dbl>      <dbl>
    ## 1 lapply      0.930  0.936      1.06        NA     52.9     5   249      4.70 
    ## 2 mtlapply(1) 0.909  0.931      1.08        NA     40.5     5   188      4.64 
    ## 3 mtlapply(2) 0.580  0.583      1.70        NA     31.7     5    93      2.93 
    ## 4 mtlapply(4) 0.304  0.308      3.25        NA     32.5     5    50      1.54 
    ## 5 mtlapply(8) 0.155  0.160      6.21        NA     33.6     5    27      0.805
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
