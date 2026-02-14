#!/usr/bin/env Rscript

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 3L) {
  stop("usage: render_dashboard.R <results_csv> <baseline_csv> <output_html>", call. = FALSE)
}

results_csv <- normalizePath(args[[1L]], winslash = "/", mustWork = TRUE)
baseline_csv <- normalizePath(args[[2L]], winslash = "/", mustWork = TRUE)
output_html <- normalizePath(args[[3L]], winslash = "/", mustWork = FALSE)
argf <- commandArgs(FALSE)
file_arg <- argf[grepl("^--file=", argf)]
if (!length(file_arg)) stop("Unable to locate script path from --file", call. = FALSE)
script_path <- sub("^--file=", "", file_arg[[1L]])
seed_root <- normalizePath(file.path(dirname(script_path), "..", ".."), mustWork = TRUE)
input_rmd <- file.path(seed_root, "benchmarks", "dashboard.Rmd")

rmarkdown::render(
  input = input_rmd,
  output_file = basename(output_html),
  output_dir = dirname(output_html),
  params = list(results_csv = results_csv, baseline_csv = baseline_csv),
  quiet = FALSE,
  envir = new.env(parent = globalenv())
)
