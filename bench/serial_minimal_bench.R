## Minimal serial-kernel benchmark artifact.
##
## Usage:
##   R --vanilla -q -f bench/serial_minimal_bench.R --args out.rds [label]
##
## Env:
## - SERIAL_MIN_ITERS: repetitions per kernel (default 5)

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

time_samples <- function(expr, iters) {
  out <- numeric(iters)
  for (i in seq_len(iters)) {
    invisible(gc())
    out[[i]] <- unname(system.time(eval(expr, parent.frame()))[["elapsed"]])
  }
  out
}

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L || length(args) > 2L) {
  stop("usage: serial_minimal_bench.R out.rds [label]", call. = FALSE)
}

out_path <- args[[1L]]
label <- if (length(args) == 2L) args[[2L]] else basename(R.home())
iters <- parse_int(Sys.getenv("SERIAL_MIN_ITERS"), 5L)
stopifnot(is.finite(iters), iters >= 1L)

kernels <- list(
  empty_for_8e7 = quote({
    for (i in 1:8e7) {}
    NULL
  }),
  scalar_add_4e7 = quote({
    s <- 0.0
    for (i in 1:4e7) s <- s + i
    s
  }),
  vector_alloc_64_x_2e6 = quote({
    s <- 0.0
    for (i in 1:2e6) {
      v <- double(64L)
      s <- s + length(v)
    }
    s
  }),
  vector_list_build_x_6e5 = quote({
    s <- 0.0
    for (i in 1:6e5) {
      v <- (i / 10) + 1:64
      a <- list(v, v * v, sqrt(v), v + 1)
      s <- s + a[[1L]][[1L]]
    }
    s
  }),
  alloc_small_sum_x_6e5 = quote({
    s <- 0.0
    for (i in 1:6e5) {
      v <- (i / 10) + 1:64
      a <- list(v, v * v, sqrt(v), v + 1)
      s <- s + sum(a[[1L]]) + sum(a[[2L]]) + sum(a[[3L]]) + sum(a[[4L]])
    }
    s
  })
)

rows <- list()
for (nm in names(kernels)) {
  ts <- time_samples(kernels[[nm]], iters = iters)
  rows[[length(rows) + 1L]] <- data.frame(
    label = label,
    kernel = nm,
    reps = iters,
    median_seconds = median(ts),
    mean_seconds = mean(ts),
    min_seconds = min(ts),
    max_seconds = max(ts),
    stringsAsFactors = FALSE
  )
}

results <- do.call(rbind, rows)
obj <- list(
  meta = list(
    label = label,
    r_version = R.version.string,
    r_home = R.home(),
    r_platform = R.version$platform,
    iters = iters
  ),
  results = results
)

dir.create(dirname(out_path), recursive = TRUE, showWarnings = FALSE)
saveRDS(obj, out_path)
cat("wrote: ", out_path, "\n", sep = "")
