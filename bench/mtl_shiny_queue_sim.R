## Simulate Shiny-like request queue pressure without running Shiny.
## Compares serial processing (lapply) vs threaded processing (mtlapply).

args <- commandArgs(trailingOnly = TRUE)
out_csv <- if (length(args) >= 1L) args[[1L]] else "bench/results/mtl_shiny_queue_sim.csv"
out_png <- if (length(args) >= 2L) args[[2L]] else "bench/figures/mtl_shiny_queue_sim.png"
ticks <- if (length(args) >= 3L) as.integer(args[[3L]]) else 80L
arrivals <- if (length(args) >= 4L) as.numeric(args[[4L]]) else 10
batch_cap <- if (length(args) >= 5L) as.integer(args[[5L]]) else 20L
thread_arg <- if (length(args) >= 6L) args[[6L]] else "2,4"
threads <- as.integer(strsplit(thread_arg, ",", fixed = TRUE)[[1L]])
threads <- threads[!is.na(threads) & threads > 0L]
if (!length(threads)) stop("no valid thread counts provided")

if (!exists("mtlapply", mode = "function")) stop("mtlapply() not available")

dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
dir.create(dirname(out_png), recursive = TRUE, showWarnings = FALSE)

work_reactive_light <- function(seed) {
  x <- seed
  for (i in seq_len(20000L)) x <- x + (i %% 7L)
  x
}

work_reactive_heavy <- function(seed) {
  x <- 0
  for (i in seq_len(90000L)) x <- x + sin(i + seed * 0.0001)
  x
}

work_reactive_mixed <- function(seed) {
  target <- proc.time()[["elapsed"]] + 0.002
  y <- seed
  while (proc.time()[["elapsed"]] < target) {
    y <- y + 1L
    if (y > 1e9) y <- seed
  }
  x <- 0
  for (i in seq_len(35000L)) x <- x + cos(i + seed * 0.0002)
  x
}

scenarios <- list(
  reactive_light = work_reactive_light,
  reactive_heavy = work_reactive_heavy,
  reactive_mixed = work_reactive_mixed
)

drain_batch <- function(queue, n_take, worker, mode, th) {
  batch <- queue[seq_len(n_take)]
  rest <- queue[-seq_len(n_take)]
  seeds <- vapply(batch, function(x) x$seed, integer(1))
  enq <- vapply(batch, function(x) x$enq, numeric(1))
  if (mode == "serial") {
    invisible(lapply(seeds, worker))
  } else {
    old <- getOption("threads")
    on.exit(options(threads = old), add = TRUE)
    options(threads = th)
    invisible(mtlapply(seeds, worker))
  }
  done <- proc.time()[["elapsed"]]
  list(rest = rest, lat = done - enq)
}

run_sim <- function(worker, mode, th, ticks, arrivals, batch_cap) {
  set.seed(20260211L)
  queue <- list()
  latencies <- numeric(0L)
  max_q <- 0L
  total_arrivals <- 0L
  t0 <- proc.time()[["elapsed"]]

  for (tick in seq_len(ticks)) {
    n_new <- rpois(1L, lambda = arrivals)
    now <- proc.time()[["elapsed"]]
    if (n_new > 0L) {
      add <- vector("list", n_new)
      for (j in seq_len(n_new)) {
        add[[j]] <- list(seed = as.integer(tick * 100000L + j), enq = now)
      }
      queue <- c(queue, add)
      total_arrivals <- total_arrivals + n_new
    }
    if (length(queue) > max_q) max_q <- length(queue)

    if (length(queue) > 0L) {
      n_take <- min(batch_cap, length(queue))
      out <- drain_batch(queue, n_take, worker, mode, th)
      queue <- out$rest
      latencies <- c(latencies, out$lat)
    }
  }

  while (length(queue) > 0L) {
    n_take <- min(batch_cap, length(queue))
    out <- drain_batch(queue, n_take, worker, mode, th)
    queue <- out$rest
    latencies <- c(latencies, out$lat)
  }

  elapsed <- proc.time()[["elapsed"]] - t0
  qs <- as.numeric(stats::quantile(latencies, probs = c(0.5, 0.95, 0.99), names = FALSE))
  data.frame(
    mode = mode,
    threads = th,
    ticks = ticks,
    arrivals_lambda = arrivals,
    batch_cap = batch_cap,
    total_arrivals = total_arrivals,
    completed = length(latencies),
    elapsed_s = elapsed,
    throughput_req_s = length(latencies) / elapsed,
    p50_latency_s = qs[[1L]],
    p95_latency_s = qs[[2L]],
    p99_latency_s = qs[[3L]],
    max_queue = max_q,
    stringsAsFactors = FALSE
  )
}

rows <- vector("list", 0L)
for (nm in names(scenarios)) {
  worker <- scenarios[[nm]]
  rows[[length(rows) + 1L]] <- cbind(scenario = nm,
                                     run_sim(worker, "serial", 1L, ticks, arrivals, batch_cap))
  for (th in threads) {
    rows[[length(rows) + 1L]] <- cbind(scenario = nm,
                                       run_sim(worker, "mtlapply", th, ticks, arrivals, batch_cap))
  }
}

res <- do.call(rbind, rows)
res$threads <- as.integer(res$threads)
baseline <- res[res$mode == "serial", c("scenario", "elapsed_s", "throughput_req_s")]
names(baseline) <- c("scenario", "serial_elapsed_s", "serial_throughput")
res <- merge(res, baseline, by = "scenario", all.x = TRUE, sort = FALSE)
res$speedup_vs_serial <- res$serial_elapsed_s / res$elapsed_s
res$throughput_gain_vs_serial <- res$throughput_req_s / res$serial_throughput
res <- res[order(res$scenario, res$mode, res$threads), ]

write.csv(res, out_csv, row.names = FALSE)

plot_df <- res[res$mode == "mtlapply", ]
labs <- paste0(plot_df$scenario, " (", plot_df$threads, "t)")
png(out_png, width = 1600, height = 900, res = 130)
par(mfrow = c(1, 2), mar = c(11, 5, 4, 1))
bp1 <- barplot(plot_df$throughput_gain_vs_serial, names.arg = labs, las = 2,
               col = "steelblue", cex.names = 0.8,
               ylab = "Throughput Gain vs Serial", main = "Queue Sim: Throughput")
abline(h = 1, lty = 2, col = "gray40")
text(bp1, plot_df$throughput_gain_vs_serial,
     labels = sprintf("%.2fx", plot_df$throughput_gain_vs_serial), pos = 3, cex = 0.75)

bp2 <- barplot(plot_df$p95_latency_s, names.arg = labs, las = 2,
               col = "darkseagreen4", cex.names = 0.8,
               ylab = "P95 Latency (s)", main = "Queue Sim: P95 Latency")
text(bp2, plot_df$p95_latency_s, labels = sprintf("%.3f", plot_df$p95_latency_s),
     pos = 3, cex = 0.75)
dev.off()

cat("Wrote:\n")
cat("  ", out_csv, "\n")
cat("  ", out_png, "\n")
cat("\nSummary:\n")
print(res[, c("scenario", "mode", "threads", "elapsed_s", "throughput_req_s",
              "p95_latency_s", "max_queue", "speedup_vs_serial",
              "throughput_gain_vs_serial")])
