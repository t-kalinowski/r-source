#!/usr/bin/env Rscript

args <- commandArgs(trailingOnly = TRUE)
out_csv <- if (length(args) >= 1L) args[[1L]] else "benchmarks/results/current.csv"

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

iters <- parse_int(Sys.getenv("TR_BENCH_ITERS"), 5L)
threads <- as.integer(strsplit(Sys.getenv("TR_BENCH_THREADS", "1,2,4,8"), ",", fixed = TRUE)[[1L]])
threads <- threads[is.finite(threads) & threads >= 1L]
if (!length(threads)) stop("No valid thread counts", call. = FALSE)
cos_outer <- parse_int(Sys.getenv("TR_BENCH_COS_OUTER"), 3e5L)
cos_k <- parse_int(Sys.getenv("TR_BENCH_COS_K"), 256L)
alloc_outer <- parse_int(Sys.getenv("TR_BENCH_ALLOC_OUTER"), 2e5L)
alloc_k <- parse_int(Sys.getenv("TR_BENCH_ALLOC_K"), 128L)

median_time <- function(expr, n = iters) {
  ts <- numeric(n)
  for (i in seq_len(n)) {
    invisible(gc())
    ts[[i]] <- unname(system.time(force(expr))[["elapsed"]])
  }
  median(ts)
}

has_mtlapply <- exists("mtlapply", mode = "function")

with_threads <- function(n, expr) {
  old <- getOption("threads")
  on.exit(options(threads = old), add = TRUE)
  options(threads = as.integer(n))
  force(expr)
}

# Representative workloads; keep these stable so trend comparisons are meaningful.
workload_cos <- function(n_outer = cos_outer, k = cos_k) {
  s <- 0
  for (i in seq_len(n_outer)) s <- s + sum(cos(seq_len(k) + i))
  s
}

workload_alloc <- function(n_outer = alloc_outer, k = alloc_k) {
  s <- 0
  for (i in seq_len(n_outer)) {
    v <- (i / 10) + seq_len(k)
    a <- list(v, v * v, sqrt(v), v + 1)
    s <- s + a[[1L]][[1L]]
  }
  s
}

run_case <- function(case_name, f) {
  rows <- list()
  l_time <- median_time(f())
  rows[[length(rows) + 1L]] <- data.frame(
    timestamp_utc = format(Sys.time(), tz = "UTC", usetz = TRUE),
    case = case_name,
    mode = "lapply-serial-proxy",
    threads = 1L,
    elapsed_s = l_time,
    stringsAsFactors = FALSE
  )

  if (has_mtlapply) {
    for (t in threads) {
      m_time <- median_time(with_threads(t, mtlapply(1:32, function(i) f())))
      rows[[length(rows) + 1L]] <- data.frame(
        timestamp_utc = format(Sys.time(), tz = "UTC", usetz = TRUE),
        case = case_name,
        mode = "mtlapply",
        threads = as.integer(t),
        elapsed_s = m_time,
        stringsAsFactors = FALSE
      )
    }
  }

  do.call(rbind, rows)
}

res <- rbind(
  run_case("cos_seq", workload_cos),
  run_case("alloc_pressure", workload_alloc)
)

if (has_mtlapply) {
  # Add explicit serial reference with lapply over shards for apples-to-apples.
  serial_rows <- list()
  shard_fun <- function(f) lapply(1:32, function(i) f())
  for (case in unique(res$case)) {
    f <- switch(case,
      cos_seq = workload_cos,
      alloc_pressure = workload_alloc,
      stop("Unknown case", call. = FALSE)
    )
    t <- median_time(shard_fun(f))
    serial_rows[[length(serial_rows) + 1L]] <- data.frame(
      timestamp_utc = format(Sys.time(), tz = "UTC", usetz = TRUE),
      case = case,
      mode = "lapply",
      threads = 1L,
      elapsed_s = t,
      stringsAsFactors = FALSE
    )
  }
  res <- rbind(res, do.call(rbind, serial_rows))
}

dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
write.csv(res, out_csv, row.names = FALSE)
cat(sprintf("wrote: %s\n", out_csv))
