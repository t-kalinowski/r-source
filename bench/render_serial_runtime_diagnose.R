## Render markdown comparison for serial runtime diagnosis artifacts.
##
## Usage:
##   R --vanilla -q -f bench/render_serial_runtime_diagnose.R --args ref.rds mtl.rds out.md

args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 3L) {
  stop("usage: render_serial_runtime_diagnose.R ref.rds mtl.rds out.md", call. = FALSE)
}

ref <- readRDS(args[[1L]])
mtl <- readRDS(args[[2L]])
out_md <- args[[3L]]

md_table <- function(df, digits = 3L) {
  out <- df
  for (nm in names(out)) {
    if (is.numeric(out[[nm]]))
      out[[nm]] <- format(round(out[[nm]], digits), nsmall = digits, trim = TRUE)
  }
  hdr <- paste0("| ", paste(names(out), collapse = " | "), " |")
  sep <- paste0("| ", paste(rep("---", ncol(out)), collapse = " | "), " |")
  body <- apply(out, 1, function(r) paste0("| ", paste(r, collapse = " | "), " |"))
  c(hdr, sep, body)
}

ref_s <- ref$summary[, c("workload", "median_elapsed")]
mtl_s <- mtl$summary[, c("workload", "median_elapsed")]
names(ref_s)[2] <- "ref_median_s"
names(mtl_s)[2] <- "mtl_median_s"
cmp <- merge(ref_s, mtl_s, by = "workload")
cmp$ratio_mtl_vs_ref <- cmp$mtl_median_s / cmp$ref_median_s
cmp$pct_diff <- (cmp$ratio_mtl_vs_ref - 1) * 100
cmp <- cmp[order(cmp$workload), ]

runtime_cols <- grep("^runtime\\.", names(mtl$runs), value = TRUE)
shared_cols <- grep("^shared\\.", names(mtl$runs), value = TRUE)

agg_nonzero <- function(df, cols) {
  if (length(cols) == 0L) return(data.frame())
  x <- colMeans(df[, cols, drop = FALSE], na.rm = TRUE)
  x <- x[is.finite(x) & x != 0]
  if (length(x) == 0L) return(data.frame())
  data.frame(counter = names(x), mean_per_run = as.numeric(x), row.names = NULL)
}

rt_tbl <- agg_nonzero(mtl$runs, runtime_cols)
sh_tbl <- agg_nonzero(mtl$runs, shared_cols)

lines <- c(
  "# Serial Runtime Diagnose",
  "",
  sprintf("Generated: %s", format(Sys.time(), "%Y-%m-%d %H:%M:%S %Z")),
  "",
  "## Build Info",
  "",
  sprintf("- ref: `%s`", ref$meta$r_home),
  sprintf("- ref version: `%s`", ref$meta$r_version),
  sprintf("- mtl: `%s`", mtl$meta$r_home),
  sprintf("- mtl version: `%s`", mtl$meta$r_version),
  sprintf("- mtl runtime stats enabled: `%s`", mtl$meta$has_stats),
  sprintf("- mtl shared-env stats enabled: `%s`", mtl$meta$has_shared_stats),
  "",
  "## Median Runtime Comparison",
  "",
  md_table(cmp[, c("workload", "ref_median_s", "mtl_median_s", "ratio_mtl_vs_ref", "pct_diff")], digits = 3L),
  ""
)

if (nrow(rt_tbl) > 0L) {
  lines <- c(
    lines,
    "## MTL Runtime Counter Means (Per Run)",
    "",
    md_table(rt_tbl[order(rt_tbl$counter), ], digits = 3L),
    ""
  )
}

if (nrow(sh_tbl) > 0L) {
  lines <- c(
    lines,
    "## MTL Shared-Env Counter Means (Per Run)",
    "",
    md_table(sh_tbl[order(sh_tbl$counter), ], digits = 3L),
    ""
  )
}

dir.create(dirname(out_md), recursive = TRUE, showWarnings = FALSE)
writeLines(lines, out_md, useBytes = TRUE)
cat("wrote: ", out_md, "\n", sep = "")
