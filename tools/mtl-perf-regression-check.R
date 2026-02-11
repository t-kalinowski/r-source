## Compare benchmark artifacts and fail on serial regressions.
##
## Usage:
##   R --vanilla -q -f tools/mtl-perf-regression-check.R --args <mtl.rds> <baseline.rds> [serial_max_ratio]

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2L || length(args) > 3L)
  stop("usage: <mtl.rds> <baseline.rds> [serial_max_ratio]", call. = FALSE)

mtl_path <- args[[1L]]
base_path <- args[[2L]]
serial_max_ratio <- if (length(args) >= 3L) as.double(args[[3L]]) else 1.10
if (!is.finite(serial_max_ratio) || serial_max_ratio <= 0)
  stop("invalid serial_max_ratio", call. = FALSE)

mtl <- readRDS(mtl_path)
base <- readRDS(base_path)

if (is.null(mtl$results) || is.null(base$results))
  stop("invalid benchmark artifact(s): expected $results", call. = FALSE)

ml <- subset(mtl$results, method == "lapply", c("workload", "median_seconds"))
bl <- subset(base$results, method == "lapply", c("workload", "median_seconds"))
names(ml)[2] <- "mtl_lapply"
names(bl)[2] <- "base_lapply"

cmp <- merge(ml, bl, by = "workload", all = FALSE, sort = TRUE)
if (!nrow(cmp)) stop("no shared lapply workloads found", call. = FALSE)

cmp$ratio <- cmp$mtl_lapply / cmp$base_lapply

cat("Serial parity check (lapply):\n")
print(cmp, row.names = FALSE)

bad <- cmp[cmp$ratio > serial_max_ratio, , drop = FALSE]
if (nrow(bad)) {
  cat("\nSerial regression detected:\n")
  print(bad, row.names = FALSE)
  stop(sprintf("serial lapply ratio exceeded threshold %.3f", serial_max_ratio),
       call. = FALSE)
}

## Optional guard: require at least two workloads to benefit with threads=4.
m4 <- subset(mtl$results, method == "mtlapply" & threads == 4L,
             c("workload", "median_seconds"))
if (nrow(m4)) {
  names(m4)[2] <- "mtlapply4"
  sp <- merge(ml, m4, by = "workload", all = FALSE, sort = TRUE)
  sp$speedup <- sp$mtl_lapply / sp$mtlapply4
  cat("\nThreaded speedup check (mtlapply threads=4 vs lapply):\n")
  print(sp, row.names = FALSE)
}

cat(sprintf("\nPASS: serial ratios <= %.3f for %d workloads\n",
            serial_max_ratio, nrow(cmp)))
