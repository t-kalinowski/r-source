
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
| alloc_pressure | mtl | lapply | 0.117 | 0.991 | 0.932 | 1.000 | NA |
| alloc_pressure | mtl | mtlapply(1) | 0.116 | 1.000 | 0.940 | 1.009 | 1.009 |
| alloc_pressure | mtl | mtlapply(2) | 0.131 | 0.885 | 0.832 | 0.893 | 0.447 |
| alloc_pressure | mtl | mtlapply(4) | 0.073 | 1.589 | 1.493 | 1.603 | 0.401 |
| alloc_pressure | mtl | mtlapply(8) | 0.046 | 2.522 | 2.370 | 2.543 | 0.318 |
| alloc_pressure | rdevel | lapply | 0.116 | 1.000 | 0.940 | 1.009 | NA |
| alloc_pressure | system | lapply | 0.109 | 1.064 | 1.000 | 1.073 | NA |
| cos_seq | mtl | lapply | 0.459 | 1.076 | 0.985 | 1.000 | NA |
| cos_seq | mtl | mtlapply(1) | 0.461 | 1.072 | 0.980 | 0.996 | 0.996 |
| cos_seq | mtl | mtlapply(2) | 0.352 | 1.403 | 1.284 | 1.304 | 0.652 |
| cos_seq | mtl | mtlapply(4) | 0.187 | 2.642 | 2.417 | 2.455 | 0.614 |
| cos_seq | mtl | mtlapply(8) | 0.105 | 4.705 | 4.305 | 4.371 | 0.546 |
| cos_seq | rdevel | lapply | 0.494 | 1.000 | 0.915 | 0.929 | NA |
| cos_seq | system | lapply | 0.452 | 1.093 | 1.000 | 1.015 | NA |
| etl_group_mean | mtl | lapply | 0.986 | 1.091 | 0.933 | 1.000 | NA |
| etl_group_mean | mtl | mtlapply(1) | 0.982 | 1.096 | 0.937 | 1.004 | 1.004 |
| etl_group_mean | mtl | mtlapply(2) | 0.619 | 1.738 | 1.486 | 1.593 | 0.796 |
| etl_group_mean | mtl | mtlapply(4) | 0.309 | 3.482 | 2.977 | 3.191 | 0.798 |
| etl_group_mean | mtl | mtlapply(8) | 0.155 | 6.942 | 5.935 | 6.361 | 0.795 |
| etl_group_mean | rdevel | lapply | 1.076 | 1.000 | 0.855 | 0.916 | NA |
| etl_group_mean | system | lapply | 0.920 | 1.170 | 1.000 | 1.072 | NA |

    ## 
    ## Single-thread overhead check (per workload):

| workload       | ratio_mtl_vs_sys | ratio_mtl_vs_rdevel |
|:---------------|-----------------:|--------------------:|
| alloc_pressure |            1.073 |               1.009 |
| cos_seq        |            1.015 |               0.929 |
| etl_group_mean |            1.072 |               0.916 |

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
