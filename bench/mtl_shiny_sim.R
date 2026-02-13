## Synthetic Shiny-like stress benchmark without running Shiny.
## Compares serial lapply() against mtlapply() on request-style workloads.

args <- commandArgs(trailingOnly = TRUE)
out_csv <- if (length(args) >= 1L) args[[1L]] else "bench/results/mtl_shiny_sim.csv"
out_png <- if (length(args) >= 2L) args[[2L]] else "bench/figures/mtl_shiny_sim_speedup.png"
requests <- if (length(args) >= 3L) as.integer(args[[3L]]) else 240L
iters <- if (length(args) >= 4L) as.integer(args[[4L]]) else 5L
thread_arg <- if (length(args) >= 5L) args[[5L]] else "2,4,8"
threads <- as.integer(strsplit(thread_arg, ",", fixed = TRUE)[[1L]])
threads <- threads[!is.na(threads) & threads > 0L]
if (!length(threads)) stop("no valid thread counts provided")

if (!exists("mtlapply", mode = "function")) {
  stop("mtlapply() is not available in this R build")
}

dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
dir.create(dirname(out_png), recursive = TRUE, showWarnings = FALSE)

work_cpu_small <- function(seed) {
  x <- 0
  for (i in seq_len(50000L)) x <- x + sin(i + seed)
  x
}

work_cpu_large <- function(seed) {
  x <- 0
  for (i in seq_len(160000L)) x <- x + cos(i + seed)
  x
}

work_delay_spin <- function(seed) {
  target <- proc.time()[["elapsed"]] + 0.01
  x <- seed
  while (proc.time()[["elapsed"]] < target) {
    x <- x + 1L
    if (x > 1e9) x <- seed
  }
  seed
}

work_mixed <- function(seed) {
  target <- proc.time()[["elapsed"]] + 0.003
  y <- seed
  while (proc.time()[["elapsed"]] < target) {
    y <- y + 1L
    if (y > 1e9) y <- seed
  }
  x <- 0
  for (i in seq_len(70000L)) x <- x + sin(i + seed * 0.001)
  x
}

scenarios <- list(
  delay_spin = work_delay_spin,
  cpu_small = work_cpu_small,
  cpu_large = work_cpu_large,
  mixed = work_mixed
)

time_lapply <- function(ids, fun, n_iter) {
  times <- numeric(n_iter)
  for (k in seq_len(n_iter)) {
    times[[k]] <- unname(system.time(invisible(lapply(ids, fun)))[["elapsed"]])
  }
  median(times)
}

time_mtlapply <- function(ids, fun, n_threads, n_iter) {
  old <- getOption("threads")
  on.exit(options(threads = old), add = TRUE)
  options(threads = n_threads)
  times <- numeric(n_iter)
  for (k in seq_len(n_iter)) {
    times[[k]] <- unname(system.time(invisible(mtlapply(ids, fun)))[["elapsed"]])
  }
  median(times)
}

ids <- seq_len(requests)
rows <- vector("list", 0L)

for (nm in names(scenarios)) {
  fun <- scenarios[[nm]]

  ## Warm up both paths to reduce first-run noise.
  invisible(lapply(ids[1:8], fun))
  old <- getOption("threads")
  options(threads = max(threads))
  invisible(mtlapply(ids[1:8], fun))
  options(threads = old)

  t_serial <- time_lapply(ids, fun, iters)
  rows[[length(rows) + 1L]] <- data.frame(
    scenario = nm,
    method = "lapply",
    threads = 1L,
    requests = requests,
    median_elapsed_s = t_serial,
    req_per_s = requests / t_serial,
    stringsAsFactors = FALSE
  )

  for (th in threads) {
    t_mt <- time_mtlapply(ids, fun, th, iters)
    rows[[length(rows) + 1L]] <- data.frame(
      scenario = nm,
      method = "mtlapply",
      threads = th,
      requests = requests,
      median_elapsed_s = t_mt,
      req_per_s = requests / t_mt,
      stringsAsFactors = FALSE
    )
  }
}

res <- do.call(rbind, rows)

baseline <- res[res$method == "lapply", c("scenario", "median_elapsed_s")]
names(baseline)[2] <- "serial_elapsed_s"
res <- merge(res, baseline, by = "scenario", all.x = TRUE, sort = FALSE)
res$speedup_vs_serial <- res$serial_elapsed_s / res$median_elapsed_s
res <- res[order(res$scenario, res$method, res$threads), ]

write.csv(res, out_csv, row.names = FALSE)

plot_df <- res[res$method == "mtlapply", ]
plot_df$label <- paste0(plot_df$scenario, " (", plot_df$threads, "t)")

png(out_png, width = 1400, height = 900, res = 130)
par(mar = c(11, 5, 4, 1))
bp <- barplot(
  plot_df$speedup_vs_serial,
  names.arg = plot_df$label,
  las = 2,
  cex.names = 0.8,
  col = "steelblue",
  ylim = c(0, max(plot_df$speedup_vs_serial) * 1.15),
  main = "Synthetic Request Benchmark: mtlapply Speedup vs lapply",
  ylab = "Speedup (serial elapsed / threaded elapsed)"
)
abline(h = 1, lty = 2, col = "gray40")
text(bp, plot_df$speedup_vs_serial, labels = sprintf("%.2fx", plot_df$speedup_vs_serial),
     pos = 3, cex = 0.75)
dev.off()

cat("Wrote:\n")
cat("  ", out_csv, "\n")
cat("  ", out_png, "\n")
cat("\nTop-line summary (median elapsed):\n")
print(res[, c("scenario", "method", "threads", "median_elapsed_s", "req_per_s", "speedup_vs_serial")])
