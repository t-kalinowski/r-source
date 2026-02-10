## README benchmark runner for experimental mtlapply()
##
## This script is base-R-only so it can be run by:
## - the system R (no mtlapply)
## - the experimental build in this repo (has mtlapply)
##
## It writes an RDS artifact with timings that can be loaded by README.Rmd.
##
## Usage:
##   R --vanilla -q -f bench/readme_bench_run.R --args bench/results/system.rds
##   ./build-mtl/bin/R --vanilla -q -f bench/readme_bench_run.R --args bench/results/mtl.rds
##
## Controls (env vars):
## - README_N: rows, default 2000000
## - README_SHARDS: tasks, default 64
## - README_GROUPS: groups, default 4096
## - README_FEAT_LOOPS: feature loops, default 40
## - README_ITERS: timing iterations, default 3
## - README_THREADS: comma-separated thread counts (mtlapply only), default "1,2,4,8"

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

parse_int_vec <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(strsplit(x, ",", fixed = TRUE)[[1L]])
}

time_median <- function(expr, iters) {
  exprq <- substitute(expr)
  ts <- numeric(iters)
  for (i in seq_len(iters)) {
    invisible(gc())
    ts[[i]] <- unname(system.time(eval(exprq, parent.frame()))[["elapsed"]])
  }
  median(ts)
}

reduce_sum_scalar <- function(parts) {
  s <- 0.0
  for (p in parts) s <- s + p
  s
}

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 1L) {
  stop("expected exactly one argument: output .rds path", call. = FALSE)
}
out_path <- args[[1L]]

N <- parse_int(Sys.getenv("README_N"), 2000000L)
nshards <- parse_int(Sys.getenv("README_SHARDS"), 64L)
ngroups <- parse_int(Sys.getenv("README_GROUPS"), 4096L)
feat_loops <- parse_int(Sys.getenv("README_FEAT_LOOPS"), 40L)
iters <- parse_int(Sys.getenv("README_ITERS"), 3L)
threads <- parse_int_vec(Sys.getenv("README_THREADS"), c(1L, 2L, 4L, 8L))

stopifnot(N >= 1L, nshards >= 1L, ngroups >= 1L, feat_loops >= 1L, iters >= 1L)
stopifnot(all(is.finite(threads)), all(threads >= 1L))

has_mtlapply <- exists("mtlapply")

## Deterministic data in the main heap (no RNG, no strings).
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)

workloads <- list()

## Workload A: ETL-ish feature engineering + group means.
workloads[["etl_group_mean"]] <- local({
  worker <- function(shard_id) {
    idx <- seq.int(shard_id, N, by = nshards)
    z <- x[idx]
    yy <- y[idx]
    ww <- w[idx]
    for (i in seq_len(feat_loops)) {
      z <- log1p(abs(z)) + sin(yy + z) * ww + cos(z - yy)
    }
    g <- grp[idx]

    ## Avoid S3 dispatch and internal helpers inside workers for now.
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

  list(ids = seq_len(nshards), worker = worker, reduce = reduce)
})

## Workload B: closure-heavy math with lots of temporary allocations.
workloads[["cos_seq"]] <- local({
  m <- 200000L
  k <- 256L

  worker <- function(shard_id) {
    is <- seq.int(shard_id, m, by = nshards)
    s <- 0.0
    for (i in is) {
      s <- s + sum(cos(seq_len(k) + i))
    }
    s
  }

  list(ids = seq_len(nshards), worker = worker, reduce = reduce_sum_scalar,
       params = list(m = m, k = k))
})

## Workload C: allocator/GC pressure without returning big objects.
workloads[["alloc_pressure"]] <- local({
  m <- 50000L
  k <- 128L

  worker <- function(shard_id) {
    is <- seq.int(shard_id, m, by = nshards)
    s <- 0.0
    for (i in is) {
      v <- (as.double(i) / 10) + seq_len(k)
      a <- list(v, v * v, sqrt(v), v + 1)
      s <- s + sum(a[[1L]]) + sum(a[[2L]]) + sum(a[[3L]]) + sum(a[[4L]])
    }
    s
  }

  list(ids = seq_len(nshards), worker = worker, reduce = reduce_sum_scalar,
       params = list(m = m, k = k))
})

results <- data.frame(
  workload = character(),
  method = character(),
  threads = integer(),
  median_seconds = double(),
  stringsAsFactors = FALSE
)

## Baseline timings first (avoid enabling worker threads before measuring lapply).
refs <- list()
for (nm in names(workloads)) {
  wl <- workloads[[nm]]
  ids <- wl$ids
  worker <- wl$worker
  reduce <- wl$reduce

  ## Correctness reference (small subset; computed in the main interpreter).
  refs[[nm]] <- reduce(lapply(ids[1:min(4L, length(ids))], worker))

  base <- time_median(reduce(lapply(ids, worker)), iters)
  results <- rbind(
    results,
    data.frame(workload = nm, method = "lapply", threads = 0L, median_seconds = base)
  )
}

## mtlapply timings (this will enable worker threads in the experimental build).
if (has_mtlapply) for (nm in names(workloads)) {
  wl <- workloads[[nm]]
  ids <- wl$ids
  worker <- wl$worker
  reduce <- wl$reduce

  cur <- reduce(mtlapply(ids[1:min(4L, length(ids))], worker, threads = max(threads)))
  stopifnot(isTRUE(all.equal(refs[[nm]], cur, tolerance = 0)))

  for (t in threads) {
    m <- time_median(reduce(mtlapply(ids, worker, threads = t)), iters)
    results <- rbind(
      results,
      data.frame(workload = nm, method = "mtlapply", threads = t, median_seconds = m)
    )
  }
}

meta <- list(
  r_version = R.version.string,
  r_platform = R.version$platform,
  r_home = R.home(),
  has_mtlapply = has_mtlapply,
  settings = list(
    N = N, nshards = nshards, ngroups = ngroups, feat_loops = feat_loops,
    iters = iters, threads = threads
  ),
  workloads = lapply(workloads, function(wl) {
    if (is.null(wl$params)) list() else wl$params
  })
)

dir.create(dirname(out_path), recursive = TRUE, showWarnings = FALSE)
saveRDS(list(meta = meta, results = results), out_path)

cat("wrote: ", out_path, "\n", sep = "")
