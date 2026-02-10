## Bench::mark artifact generator for README plotting.
##
## This script is intended to run under the experimental build (has mtlapply)
## and writes an RDS artifact that README.Rmd can load and plot.
##
## Usage:
##   ./build-mtl/bin/R --vanilla -q -f bench/readme_bench_mark.R --args bench/results/mtl_bench_mark.rds
##
## Controls (env vars, same defaults as bench/readme_bench_run.R):
## - README_N
## - README_SHARDS
## - README_GROUPS
## - README_FEAT_LOOPS
## - README_ITERS
## - README_THREADS (comma-separated; default "1,2,4,8")

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

parse_int_vec <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(strsplit(x, ",", fixed = TRUE)[[1L]])
}

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 1L) {
  stop("expected exactly one argument: output .rds path", call. = FALSE)
}
out_path <- args[[1L]]

stopifnot(exists("mtlapply"))
stopifnot(requireNamespace("bench", quietly = TRUE))

N <- parse_int(Sys.getenv("README_N"), 2000000L)
nshards <- parse_int(Sys.getenv("README_SHARDS"), 64L)
ngroups <- parse_int(Sys.getenv("README_GROUPS"), 4096L)
feat_loops <- parse_int(Sys.getenv("README_FEAT_LOOPS"), 40L)
iters <- parse_int(Sys.getenv("README_ITERS"), 3L)
threads <- parse_int_vec(Sys.getenv("README_THREADS"), c(1L, 2L, 4L, 8L))

stopifnot(N >= 1L, nshards >= 1L, ngroups >= 1L, feat_loops >= 1L, iters >= 1L)
stopifnot(all(is.finite(threads)), all(threads >= 1L))

## Deterministic data in the main heap (no RNG, no strings).
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)

ids <- seq_len(nshards)

worker <- function(shard_id) {
  idx <- seq.int(shard_id, N, by = nshards)
  z <- x[idx]
  yy <- y[idx]
  ww <- w[idx]
  for (i in seq_len(feat_loops)) {
    z <- log1p(abs(z)) + sin(yy + z) * ww + cos(z - yy)
  }
  g <- grp[idx]

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

## Correctness check (small subset; computed in the main interpreter).
ref <- reduce(lapply(ids[1:min(4L, length(ids))], worker))
cur <- reduce(mtlapply(ids[1:min(4L, length(ids))], worker, threads = max(threads)))
stopifnot(isTRUE(all.equal(ref, cur, tolerance = 0)))

## One workload for plotting: lapply vs mtlapply scaling.
exprs <- list(lapply = quote(reduce(lapply(ids, worker))))
for (t in threads) {
  exprs[[paste0("mtlapply(", t, ")")]] <- substitute(
    reduce(mtlapply(ids, worker, threads = TT)),
    list(TT = t)
  )
}

res <- do.call(
  bench::mark,
  c(
    exprs,
    list(iterations = iters, check = FALSE, time_unit = "s")
  )
)

meta <- list(
  r_version = R.version.string,
  r_platform = R.version$platform,
  r_home = R.home(),
  settings = list(
    N = N, nshards = nshards, ngroups = ngroups, feat_loops = feat_loops,
    iters = iters, threads = threads
  )
)

dir.create(dirname(out_path), recursive = TRUE, showWarnings = FALSE)
saveRDS(list(meta = meta, bench = res), out_path)
cat("wrote: ", out_path, "\n", sep = "")

