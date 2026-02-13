#!/usr/bin/env Rscript

args <- commandArgs(trailingOnly = TRUE)

threads <- if (length(args) >= 1L) as.integer(args[[1L]]) else 8L
reps <- if (length(args) >= 2L) as.integer(args[[2L]]) else 3L
min_eff <- if (length(args) >= 3L) as.numeric(args[[3L]]) else 0.50

if (is.na(threads) || threads < 2L) {
  stop("threads must be >= 2", call. = FALSE)
}
if (is.na(reps) || reps < 1L) {
  stop("reps must be >= 1", call. = FALSE)
}
if (is.na(min_eff) || min_eff <= 0 || min_eff > 1) {
  stop("min_eff must be in (0, 1]", call. = FALSE)
}

thread_cap_vars <- c(
  "VECLIB_MAXIMUM_THREADS",
  "OMP_NUM_THREADS",
  "OPENBLAS_NUM_THREADS",
  "MKL_NUM_THREADS",
  "BLIS_NUM_THREADS",
  "NUMEXPR_NUM_THREADS"
)
for (nm in thread_cap_vars) {
  do.call(Sys.setenv, setNames(list("1"), nm))
}

run_case <- function(name, rows, cols, n, threads, reps) {
  A <- matrix(runif(rows * cols), rows, cols)
  B <- matrix(runif(rows * cols), rows, cols)
  old_threads <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old_threads), add = TRUE)
  options(mtlapply.threads = as.integer(threads))

  l_times <- numeric(reps)
  m_times <- numeric(reps)

  invisible((A %*% B) + 1)
  invisible(mtlapply(1:1, function(i) (A %*% B) + i))

  for (i in seq_len(reps)) {
    gc()
    l_times[[i]] <- unname(system.time(lapply(1:n, function(j) (A %*% B) + j))[["elapsed"]])
    gc()
    m_times[[i]] <- unname(system.time(mtlapply(1:n, function(j) (A %*% B) + j))[["elapsed"]])
  }

  l_med <- median(l_times)
  m_med <- median(m_times)
  speedup <- l_med / m_med
  efficiency <- speedup / threads

  data.frame(
    case = name,
    rows = rows,
    cols = cols,
    n = n,
    threads = threads,
    reps = reps,
    lapply_median_s = l_med,
    mtlapply_median_s = m_med,
    speedup = speedup,
    efficiency = efficiency,
    stringsAsFactors = FALSE
  )
}

cases <- list(
  list(name = "matmul_1000_n20", rows = 1000L, cols = 1000L, n = 20L),
  list(name = "matmul_100_n20000", rows = 100L, cols = 100L, n = 20000L)
)

results <- do.call(
  rbind,
  lapply(cases, function(x) {
    run_case(x$name, x$rows, x$cols, x$n, threads = threads, reps = reps)
  })
)

print(results, row.names = FALSE, digits = 4)

min_speedup <- threads * min_eff
bad <- results$speedup < min_speedup
if (any(bad)) {
  bad_rows <- results[bad, c("case", "speedup", "efficiency")]
  msg <- paste(
    sprintf("threadpool speedup smoke failed: expected speedup >= %.3f (threads=%d, min_eff=%.2f)", min_speedup, threads, min_eff),
    paste(apply(bad_rows, 1L, function(r) sprintf("%s: speedup=%.3f efficiency=%.3f", r[[1L]], as.numeric(r[[2L]]), as.numeric(r[[3L]]))), collapse = "; "),
    sep = "\n"
  )
  stop(msg, call. = FALSE)
}

cat(sprintf("\nthreadpool speedup smoke ok (min speedup %.3f, threads=%d, min_eff=%.2f)\n", min_speedup, threads, min_eff))
