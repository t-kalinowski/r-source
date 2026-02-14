## Compare minimal serial-kernel benchmark artifacts and fail on regressions.
##
## Usage:
##   R --vanilla -q -f tools/mtl-serial-minimal-check.R --args \
##     <mtl_minimal.rds> <baseline_minimal.rds> [max_ratio]

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2L || length(args) > 3L) {
  stop("usage: <mtl_minimal.rds> <baseline_minimal.rds> [max_ratio]", call. = FALSE)
}

mtl_path <- args[[1L]]
base_path <- args[[2L]]
max_ratio <- if (length(args) >= 3L) as.double(args[[3L]]) else 1.10
if (!is.finite(max_ratio) || max_ratio <= 0) {
  stop("invalid max_ratio", call. = FALSE)
}

read_tbl <- function(path, nm) {
  x <- readRDS(path)
  if (!is.list(x) || is.null(x$results) || !is.data.frame(x$results)) {
    stop("invalid benchmark artifact: ", path, call. = FALSE)
  }
  out <- x$results[, c("kernel", "median_seconds"), drop = FALSE]
  names(out)[2L] <- nm
  out
}

mtl <- read_tbl(mtl_path, "mtl_seconds")
base <- read_tbl(base_path, "baseline_seconds")
cmp <- merge(mtl, base, by = "kernel", all = FALSE, sort = TRUE)
if (!nrow(cmp)) stop("no shared kernels between artifacts", call. = FALSE)

cmp$ratio_mtl_vs_baseline <- cmp$mtl_seconds / cmp$baseline_seconds
cmp$pct_diff <- (cmp$ratio_mtl_vs_baseline - 1) * 100

cat("Minimal serial-kernel parity:\n")
print(cmp, row.names = FALSE)

bad <- cmp[cmp$ratio_mtl_vs_baseline > max_ratio, , drop = FALSE]
if (nrow(bad)) {
  cat("\nSerial-kernel regressions detected:\n")
  print(bad, row.names = FALSE)
  stop(sprintf("minimal serial ratio exceeded threshold %.3f", max_ratio), call. = FALSE)
}

cat(sprintf("\nPASS: all minimal kernels ratio <= %.3f\n", max_ratio))
