## Shiny-style burst benchmark using background()/wait() on the shared thread pool.
##
## Compares:
## - serial request handling (lapply-style)
## - batch parallel handling (mtlapply)
## - async queue handling (background + wait)
##
## Usage:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-shiny-background-smoke.R --args \
##     [out_csv] [out_png] [requests] [iters] [threads_csv] [min_bg_speedup] [work_iters]

args <- commandArgs(trailingOnly = TRUE)
out_csv <- if (length(args) >= 1L) args[[1L]] else "bench/results/mtl_shiny_background_smoke.csv"
out_png <- if (length(args) >= 2L) args[[2L]] else "bench/figures/mtl_shiny_background_smoke.png"
requests <- if (length(args) >= 3L) as.integer(args[[3L]]) else 96L
iters <- if (length(args) >= 4L) as.integer(args[[4L]]) else 3L
threads_csv <- if (length(args) >= 5L) args[[5L]] else "2,4,8"
min_bg_speedup <- if (length(args) >= 6L) as.numeric(args[[6L]]) else 1.5
work_iters <- if (length(args) >= 7L) as.integer(args[[7L]]) else 120000L

threads <- as.integer(strsplit(threads_csv, ",", fixed = TRUE)[[1L]])
threads <- sort(unique(threads[is.finite(threads) & threads >= 1L]))

if (!exists("background", mode = "function") ||
    !exists("wait", mode = "function") ||
    !exists("mtlapply", mode = "function")) {
  stop("background()/wait()/mtlapply() are required in this build", call. = FALSE)
}
if (!is.finite(requests) || requests < 1L) stop("invalid requests", call. = FALSE)
if (!is.finite(iters) || iters < 1L) stop("invalid iters", call. = FALSE)
if (!length(threads)) stop("no valid thread counts", call. = FALSE)
if (!is.finite(min_bg_speedup) || min_bg_speedup <= 0) stop("invalid min_bg_speedup", call. = FALSE)
if (!is.finite(work_iters) || work_iters < 1000L) stop("invalid work_iters", call. = FALSE)

dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
dir.create(dirname(out_png), recursive = TRUE, showWarnings = FALSE)

## Deterministic CPU-heavy request handler (no shared mutable state).
request_work <- function(seed) {
  x <- as.double(seed) / 10
  for (i in seq_len(work_iters)) {
    x <- x + sin(i + x) + cos(i - x * 0.5)
  }
  x
}

run_serial <- function(ids) {
  n <- length(ids)
  done <- numeric(n)
  out <- numeric(n)
  t0 <- proc.time()[["elapsed"]]
  for (i in seq_along(ids)) {
    out[[i]] <- request_work(ids[[i]])
    done[[i]] <- proc.time()[["elapsed"]] - t0
  }
  list(elapsed = done[[n]], done = done, out = out)
}

run_mtlapply <- function(ids, n_threads) {
  old <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old), add = TRUE)
  options(mtlapply.threads = as.integer(n_threads))

  t0 <- proc.time()[["elapsed"]]
  out <- unlist(mtlapply(ids, request_work), use.names = FALSE)
  elapsed <- proc.time()[["elapsed"]] - t0
  list(elapsed = elapsed, out = out)
}

run_background <- function(ids, n_threads) {
  old <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old), add = TRUE)
  options(mtlapply.threads = as.integer(n_threads))

  n <- length(ids)
  done <- numeric(n)
  out <- numeric(n)

  t0 <- proc.time()[["elapsed"]]
  active <- lapply(ids, function(i) background(request_work(i)))
  active_ids <- ids

  while (length(active)) {
    got <- wait(active)
    idx_attr <- attr(got, "index")
    if (!length(idx_attr)) {
      stop("background wait returned no index", call. = FALSE)
    }
    idx <- as.integer(idx_attr[[1L]])
    if (!is.finite(idx) || idx < 1L || idx > length(active_ids)) {
      stop("background wait returned invalid index", call. = FALSE)
    }
    req_id <- active_ids[[idx]]
    if (!isTRUE(attr(got, "ok"))) {
      stop("background request failed", call. = FALSE)
    }
    out[[req_id]] <- got$value
    done[[req_id]] <- proc.time()[["elapsed"]] - t0
    active <- active[-idx]
    active_ids <- active_ids[-idx]
  }

  list(elapsed = max(done), done = done, out = out)
}

summarize_run <- function(mode, threads, elapsed, done) {
  p <- if (is.null(done)) c(NA_real_, NA_real_, NA_real_) else as.numeric(stats::quantile(
    done, probs = c(0.5, 0.95, 0.99), names = FALSE
  ))
  data.frame(
    mode = mode,
    threads = as.integer(threads),
    elapsed_s = elapsed,
    throughput_req_s = requests / elapsed,
    p50_s = p[[1L]],
    p95_s = p[[2L]],
    p99_s = p[[3L]],
    stringsAsFactors = FALSE
  )
}

ids <- seq_len(requests)
rows <- vector("list", 0L)

## Warm up all paths.
invisible(run_serial(ids[1:8]))
invisible(run_mtlapply(ids[1:8], max(threads)))
invisible(run_background(ids[1:8], max(threads)))

for (k in seq_len(iters)) {
  ser <- run_serial(ids)
  rows[[length(rows) + 1L]] <- cbind(iter = k, summarize_run("serial", 1L, ser$elapsed, ser$done))

  for (th in threads) {
    mt <- run_mtlapply(ids, th)
    stopifnot(isTRUE(all.equal(ser$out, mt$out, tolerance = 1e-10)))
    rows[[length(rows) + 1L]] <- cbind(iter = k, summarize_run("mtlapply", th, mt$elapsed, NULL))

    bg <- run_background(ids, th)
    stopifnot(isTRUE(all.equal(ser$out, bg$out, tolerance = 1e-10)))
    rows[[length(rows) + 1L]] <- cbind(iter = k, summarize_run("background", th, bg$elapsed, bg$done))
  }
}

raw <- do.call(rbind, rows)

agg <- aggregate(
  cbind(elapsed_s, throughput_req_s, p50_s, p95_s, p99_s) ~ mode + threads,
  data = raw, FUN = median
)
agg <- agg[order(agg$mode, agg$threads), ]

serial_elapsed <- agg$elapsed_s[agg$mode == "serial" & agg$threads == 1L][[1L]]
serial_p95 <- agg$p95_s[agg$mode == "serial" & agg$threads == 1L][[1L]]
agg$speedup_vs_serial <- serial_elapsed / agg$elapsed_s
agg$p95_gain_vs_serial <- serial_p95 / agg$p95_s

write.csv(agg, out_csv, row.names = FALSE)

plot_df <- agg[agg$mode != "serial", ]
labs <- paste0(plot_df$mode, " (", plot_df$threads, "t)")

png(out_png, width = 1400, height = 900, res = 130)
par(mfrow = c(1, 2), mar = c(11, 5, 4, 1))
bp1 <- barplot(plot_df$speedup_vs_serial, names.arg = labs, las = 2, cex.names = 0.8,
               col = "steelblue", ylab = "Speedup vs serial", main = "Burst Throughput Speedup")
abline(h = 1, lty = 2, col = "gray40")
text(bp1, plot_df$speedup_vs_serial, labels = sprintf("%.2fx", plot_df$speedup_vs_serial),
     pos = 3, cex = 0.75)

bg_df <- agg[agg$mode == "background", ]
bp2 <- barplot(bg_df$p95_gain_vs_serial, names.arg = paste0(bg_df$threads, "t"), las = 2,
               col = "darkseagreen4", ylab = "P95 gain vs serial", main = "Burst P95 Latency Gain")
abline(h = 1, lty = 2, col = "gray40")
text(bp2, bg_df$p95_gain_vs_serial, labels = sprintf("%.2fx", bg_df$p95_gain_vs_serial),
     pos = 3, cex = 0.75)
dev.off()

max_th <- max(threads)
bg_max <- agg[agg$mode == "background" & agg$threads == max_th, , drop = FALSE]
if (!nrow(bg_max)) stop("missing background row for max thread count", call. = FALSE)
if (bg_max$speedup_vs_serial[[1L]] < min_bg_speedup) {
  stop(sprintf(
    "background speedup check failed: %.3fx < required %.3fx (threads=%d)",
    bg_max$speedup_vs_serial[[1L]], min_bg_speedup, max_th
  ), call. = FALSE)
}

cat("wrote:\n")
cat("  ", out_csv, "\n", sep = "")
cat("  ", out_png, "\n", sep = "")
cat("\nmedian summary:\n")
print(agg)
cat(sprintf(
  "\nPASS: background speedup %.3fx at %d threads (threshold %.3fx)\n",
  bg_max$speedup_vs_serial[[1L]], max_th, min_bg_speedup
))
