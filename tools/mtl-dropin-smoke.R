## Smoke test for "drop-in" package compatibility of the MTL shared-library
## build on macOS.
##
## This exercises loading common binary packages that are typically linked
## against R.framework's libR.dylib and validates a few representative calls.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) > 1L) {
  stop("usage: [threads]", call. = FALSE)
}

threads <- if (length(args) == 1L) as.integer(args[[1L]]) else 4L
if (!is.finite(threads) || threads < 1L) stop("invalid threads", call. = FALSE)

cat("R.home(): ", R.home(), "\n", sep = "")
cat(".libPaths():\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

suppressPackageStartupMessages(library(Matrix))
cat("Matrix ok\n")

suppressPackageStartupMessages(library(reticulate))
cat("reticulate ok\n")

suppressPackageStartupMessages(library(dplyr))
suppressPackageStartupMessages(library(tibble))

df <- tibble(g = c("a", "a", "b", "b"), x = 1:4)
out <- df |>
  group_by(g) |>
  summarise(s = sum(x), .groups = "drop") |>
  arrange(g)
stopifnot(identical(as.character(out$g), c("a", "b")))
stopifnot(identical(as.integer(out$s), c(3L, 7L)))
cat("dplyr ok\n")

if (exists("mtlapply", mode = "function")) {
  old_threads <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old_threads), add = TRUE)
  options(mtlapply.threads = threads)
  got <- mtlapply(1:16, function(i) {
    i * 9L
  })
  ref <- lapply(1:16, function(i) i * 9L)
  stopifnot(identical(got, ref))
  cat("mtlapply worker path ok\n")
}

cat("\ndrop-in smoke ok\n")
