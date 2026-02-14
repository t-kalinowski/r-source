#!/usr/bin/env Rscript

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2L) {
  stop("usage: check_thresholds.R <results_csv> <baseline_csv> [max_serial_ratio] [min_efficiency]", call. = FALSE)
}

results_csv <- args[[1L]]
baseline_csv <- args[[2L]]
max_serial_ratio <- if (length(args) >= 3L) as.numeric(args[[3L]]) else 1.05
min_eff <- if (length(args) >= 4L) as.numeric(args[[4L]]) else 0.50

res <- read.csv(results_csv, stringsAsFactors = FALSE)
base <- read.csv(baseline_csv, stringsAsFactors = FALSE)

if (!all(c("case", "mode", "threads", "elapsed_s") %in% names(res))) {
  stop("results CSV missing required columns", call. = FALSE)
}
if (!all(c("case", "baseline_serial_elapsed_s") %in% names(base))) {
  stop("baseline CSV missing required columns", call. = FALSE)
}

serial <- subset(res, mode %in% c("lapply", "lapply-serial-proxy"))
if (!nrow(serial)) stop("no serial rows in results", call. = FALSE)

serial_med <- aggregate(elapsed_s ~ case, serial, median)
names(serial_med)[2] <- "current_serial"
cmp <- merge(serial_med, base[, c("case", "baseline_serial_elapsed_s")], by = "case", all = FALSE)
if (!nrow(cmp)) stop("no overlapping baseline/serial cases", call. = FALSE)

cmp$ratio <- cmp$current_serial / cmp$baseline_serial_elapsed_s
bad_serial <- subset(cmp, is.finite(ratio) & ratio > max_serial_ratio)

if (nrow(bad_serial)) {
  print(bad_serial)
  stop(sprintf("serial parity regression: ratio > %.3f", max_serial_ratio), call. = FALSE)
}

thr <- subset(res, mode == "mtlapply")
if (nrow(thr)) {
  lref <- subset(res, mode == "lapply", c("case", "elapsed_s"))
  names(lref)[2] <- "serial_elapsed"
  sp <- merge(thr, lref, by = "case", all.x = TRUE)
  sp$speedup <- sp$serial_elapsed / sp$elapsed_s
  sp$efficiency <- sp$speedup / sp$threads

  bad_eff <- subset(sp, is.finite(efficiency) & threads > 1L & efficiency < min_eff)
  if (nrow(bad_eff)) {
    print(bad_eff[, c("case", "threads", "speedup", "efficiency")])
    stop(sprintf("threaded efficiency regression: efficiency < %.3f", min_eff), call. = FALSE)
  }
}

cat("threshold checks passed\n")
