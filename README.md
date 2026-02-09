# R Multi-Threaded Interpreter Experiment (`mtlapply`)

This repo is an experimental R runtime that can evaluate *pure R* closures concurrently on multiple OS threads, in a single R process, using `mtlapply()` (modeled after `lapply()`/`mclapply()`).

The “hook”: you can run a fairly realistic “ETL / feature engineering / group-summarise” pipeline with near-linear speedups, without forking and without serializing return values.

## Benchmark: “Shard -> Mutate -> Summarise -> Top-k -> Reduce”

This benchmark is base-R-only, but deliberately shaped like a dplyr workflow:

1. shard rows into tasks
2. `mutate`: vectorized feature engineering (alloc-heavy numeric transforms)
3. `summarise`: group-wise aggregation
4. `slice_max`: compute a small per-shard top-k
5. reduce results back on the main thread

Run it (from repo root, using the `build-mtl` build):

```sh
MTL_ETL_N=2000000 \
MTL_ETL_SHARDS=64 \
MTL_ETL_GROUPS=4096 \
MTL_ETL_FEAT_LOOPS=40 \
MTL_ETL_ITERS=3 \
MTL_ETL_THREADS=1,2,4,8 \
./build-mtl/bin/R --vanilla -q -f bench/mtlapply_etl.R
```

Example output (this machine):

```text
== ETL pipeline: shard -> mutate -> summarise -> top-k -> reduce ==
lapply          median=   0.967
mtlapply(1)     median=   0.929  speedup= 1.04x
mtlapply(2)     median=   0.464  speedup= 2.08x
mtlapply(4)     median=   0.232  speedup= 4.17x
mtlapply(8)     median=   0.118  speedup= 8.19x
```

### Same Benchmark As “Real R Code” (`bench::mark`)

If you have the `bench` package installed, you can run essentially the same
workflow directly from an R session:

```r
# install.packages("bench")
library(bench)

N <- 2e6L
shards <- 64L
ngroups <- 4096L
feat_loops <- 40L

# Deterministic data (no RNG, no strings).
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)

idxs <- lapply(seq_len(shards), function(k) seq.int(k, N, by = shards))

process_shard <- function(idx) {
  z <- x[idx]
  yy <- y[idx]
  ww <- w[idx]
  for (i in seq_len(feat_loops)) {
    z <- log1p(abs(z)) + sin(yy + z) * ww + cos(z - yy)
  }

  g <- grp[idx]
  s1 <- rowsum.default(z, g, reorder = FALSE)
  cnt <- tabulate(g, ngroups)
  top <- sort.int(z, decreasing = TRUE, method = "quick")[1:100]

  list(s1 = as.double(s1), cnt = cnt, top = top)
}

reduce_results <- function(res) {
  s1 <- Reduce(`+`, lapply(res, `[[`, "s1"))
  cnt <- Reduce(`+`, lapply(res, `[[`, "cnt"))
  mu <- s1 / cnt
  top_all <- sort.int(unlist(lapply(res, `[[`, "top"), use.names = FALSE),
                     decreasing = TRUE, method = "quick")[1:100]
  list(mu = mu, top = top_all)
}

res <- bench::mark(
  lapply = reduce_results(lapply(idxs, process_shard)),
  mtl_2  = reduce_results(mtlapply(idxs, process_shard, threads = 2L)),
  mtl_4  = reduce_results(mtlapply(idxs, process_shard, threads = 4L)),
  mtl_8  = reduce_results(mtlapply(idxs, process_shard, threads = 8L)),
  iterations = 5,
  check = TRUE
)

print(res)
plot(res)
```

## Quick Demo

In an R session built from this tree:

```r
mtlapply(1:100, \(i) cos(seq_len(i)), threads = 8L)
```

## What This Tries To Be

- One R process.
- A thread pool of worker “sub-interpreters”.
- Workers evaluate R bytecode/interpreter code in parallel for most pure R work.
- Workers allocate into worker-local heaps/GC, and results are transferred back without serialization.

## Current Limitations (Important)

- Workloads that *hammer global mutable runtime structures* (notably string/symbol interning) may scale poorly.
- Some operations are explicitly serialized under a global lock when run from workers (notably `.External` and selected native interfaces), to avoid corrupting global process state.
- This is experimental runtime work: correctness and safety come before “make everything parallel”.

## Files

- `bench/mtlapply_etl.R`: the “real-ish” benchmark above.
- `tests/mtlapply.R`: small regression tests for `mtlapply()`.
- `tests/mtlstress.R`: randomized stress testing (not part of `make check`).
