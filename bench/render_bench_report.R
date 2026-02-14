## Render benchmark artifacts into a committed markdown summary.
##
## Usage:
##   build-mtl-shlib/bin/R --vanilla -q -f bench/render_bench_report.R --args [out_md]
##
## Defaults:
##   out_md = bench/LATEST.md

args <- commandArgs(trailingOnly = TRUE)
out_md <- if (length(args) >= 1L) args[[1L]] else "bench/LATEST.md"

require_file <- function(path) {
  if (!file.exists(path)) {
    stop("missing required benchmark artifact: ", path, call. = FALSE)
  }
  normalizePath(path, winslash = "/", mustWork = TRUE)
}

read_results <- function(path) {
  x <- readRDS(path)
  if (!is.list(x) || is.null(x$results) || !is.data.frame(x$results)) {
    stop("invalid benchmark artifact: ", path, call. = FALSE)
  }
  x$results
}

md_num <- function(x, digits = 3L) {
  out <- character(length(x))
  na <- is.na(x)
  out[na] <- "NA"
  out[!na] <- sprintf(paste0("%.", digits, "f"), as.numeric(x[!na]))
  out
}

md_table <- function(df, digits = 3L) {
  if (!nrow(df)) return("(no rows)\n")
  cols <- names(df)
  header <- paste0("| ", paste(cols, collapse = " | "), " |")
  sep <- paste0("| ", paste(rep("---", length(cols)), collapse = " | "), " |")
  body <- apply(df, 1L, function(row) {
    vals <- character(length(row))
    for (i in seq_along(row)) {
      val <- row[[i]]
      if (is.na(val)) {
	vals[[i]] <- "NA"
      } else if (inherits(df[[i]], "integer")) {
	vals[[i]] <- as.character(as.integer(val))
      } else if (is.numeric(df[[i]])) {
	vals[[i]] <- md_num(as.numeric(val), digits = digits)
      } else {
	vals[[i]] <- as.character(val)
      }
    }
    paste0("| ", paste(vals, collapse = " | "), " |")
  })
  paste(c(header, sep, body), collapse = "\n")
}

system_path <- require_file("bench/results/system_latest.rds")
rdevel_path <- require_file("bench/results/rdevel_latest.rds")
mtl_path <- require_file("bench/results/mtl_latest.rds")
threadpool_path <- require_file("bench/results/threadpool_perf_checkpoint.csv")
bg_path <- require_file("bench/results/mtl_shiny_background_smoke_checkpoint.csv")
serial_min_ref_path <- require_file("bench/results/serial_minimal_rdevel_latest.rds")
serial_min_mtl_path <- require_file("bench/results/serial_minimal_mtl_latest.rds")
shiny_full_path <- "bench/results/shiny_scale300_w8_full.csv"

system_res <- read_results(system_path)
rdevel_res <- read_results(rdevel_path)
mtl_res <- read_results(mtl_path)
threadpool_res <- read.csv(threadpool_path, stringsAsFactors = FALSE)
bg_res <- read.csv(bg_path, stringsAsFactors = FALSE)
serial_min_ref <- read_results(serial_min_ref_path)
serial_min_mtl <- read_results(serial_min_mtl_path)
shiny_full <- if (file.exists(shiny_full_path)) {
  read.csv(shiny_full_path, stringsAsFactors = FALSE)
} else {
  data.frame()
}

lapply_table <- function(lhs, rhs, lhs_name, rhs_name) {
  l <- subset(lhs, method == "lapply", c("workload", "median_seconds"))
  r <- subset(rhs, method == "lapply", c("workload", "median_seconds"))
  names(l)[2] <- lhs_name
  names(r)[2] <- rhs_name
  z <- merge(l, r, by = "workload", all = FALSE, sort = TRUE)
  z$ratio <- z[[lhs_name]] / z[[rhs_name]]
  z
}

parity_rdevel <- lapply_table(mtl_res, rdevel_res, "mtl_lapply_s", "rdevel_lapply_s")
parity_system <- lapply_table(mtl_res, system_res, "mtl_lapply_s", "system_lapply_s")

serial_min <- merge(
  serial_min_mtl[, c("kernel", "median_seconds"), drop = FALSE],
  serial_min_ref[, c("kernel", "median_seconds"), drop = FALSE],
  by = "kernel", all = FALSE, sort = TRUE
)
names(serial_min)[2:3] <- c("mtl_s", "rdevel_s")
serial_min$ratio <- serial_min$mtl_s / serial_min$rdevel_s
serial_min$pct_diff <- (serial_min$ratio - 1) * 100

ml <- subset(mtl_res, method == "lapply", c("workload", "median_seconds"))
names(ml)[2] <- "lapply_s"
mm <- subset(mtl_res, method == "mtlapply", c("workload", "threads", "median_seconds"))
names(mm)[3] <- "mtlapply_s"
scale_tab <- merge(mm, ml, by = "workload", all.x = TRUE, sort = TRUE)
scale_tab$speedup <- scale_tab$lapply_s / scale_tab$mtlapply_s
scale_tab$efficiency <- scale_tab$speedup / scale_tab$threads
scale_tab <- scale_tab[order(scale_tab$workload, scale_tab$threads), ]

lines <- c(
  "# Benchmark Artifact Report",
  "",
  "This file is generated from committed artifacts in `bench/results/`.",
  "Regenerate with `tools/mtl-bench-refresh.sh` and inspect with `git diff bench/LATEST.md README.md bench/results/`.",
  "",
  "## Serial Parity vs R-devel (`ratio = mtl / rdevel`)",
  "",
  md_table(parity_rdevel, digits = 3L),
  "",
  "## Serial Parity vs System R (`ratio = mtl / system`)",
  "",
  md_table(parity_system, digits = 3L),
  "",
  "## Minimal Serial Kernels vs R-devel (`ratio = mtl / rdevel`)",
  "",
  md_table(serial_min, digits = 3L),
  "",
  "## `mtlapply` Scaling (from `bench/results/mtl_latest.rds`)",
  "",
  md_table(scale_tab[, c("workload", "threads", "lapply_s", "mtlapply_s", "speedup", "efficiency")], digits = 3L),
  "",
  "## Threadpool Matrix Benchmark",
  "",
  md_table(threadpool_res, digits = 3L),
  "",
  "## `background()` / `wait()` Burst Benchmark",
  "",
  md_table(bg_res, digits = 3L)
)

if (nrow(shiny_full)) {
  lines <- c(
    lines,
    "",
    "## Shiny Full Loadtest Artifact",
    "",
    md_table(shiny_full, digits = 3L)
  )
}

dir.create(dirname(out_md), recursive = TRUE, showWarnings = FALSE)
writeLines(lines, out_md, useBytes = TRUE)
cat("wrote: ", out_md, "\n", sep = "")
