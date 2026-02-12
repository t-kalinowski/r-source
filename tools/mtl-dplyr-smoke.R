## Smoke test: load dplyr from an existing binary package library and run a few
## representative operations.
##
## Usage:
##   build-mtl/bin/R --vanilla -q -f tools/mtl-dplyr-smoke.R --args \
##     /Users/tomasz/Library/R/arm64/4.6/library [threads]
##
## This is intentionally base-R-only (no testthat).

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L || length(args) > 2L)
  stop("usage: <libpath> [threads]", call. = FALSE)

lib <- args[[1L]]
threads <- if (length(args) >= 2L) as.integer(args[[2L]]) else 4L
if (!is.finite(threads) || threads < 1L) stop("invalid threads", call. = FALSE)

if (!dir.exists(lib)) stop("library path does not exist: ", lib, call. = FALSE)

.libPaths(c(lib, .libPaths()))
cat("libPaths:\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

suppressPackageStartupMessages(library(dplyr))
suppressPackageStartupMessages(library(tibble))
suppressPackageStartupMessages(library(rlang))
suppressPackageStartupMessages(library(vctrs))

## Main-thread examples.
df <- tibble(x = 1:5, g = c("a", "a", "b", "b", "b"))

out1 <- df |>
  group_by(g) |>
  summarise(s = sum(x), .groups = "drop") |>
  arrange(g)
stopifnot(identical(as.character(out1$g), c("a", "b")))
stopifnot(identical(as.integer(out1$s), c(3L, 12L)))

out2 <- df |>
  mutate(y = x * 2L + 1L) |>
  filter(y > 5L) |>
  arrange(desc(y))
stopifnot(identical(as.integer(out2$y), c(11L, 9L, 7L)))

lhs <- tibble(id = 1:3, v = c(10, 20, 30))
rhs <- tibble(id = c(2, 3, 4), w = c(200, 300, 400))
j <- left_join(lhs, rhs, by = "id") |>
  arrange(id)
stopifnot(is.na(j$w[[1L]]))
stopifnot(identical(as.integer(j$w[[2L]]), 200L))
stopifnot(identical(as.integer(j$w[[3L]]), 300L))

## Optional (opt-in): exercise dplyr/vctrs/rlang code on worker threads too.
## This is intentionally gated because dplyr worker execution is still an active
## compatibility target and can be flaky while runtime internals evolve.
if (exists("mtlapply") && identical(Sys.getenv("MTL_DPLYR_WORKER", "0"), "1")) {
  old_threads <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old_threads), add = TRUE)
  options(mtlapply.threads = threads)

  rpc_reset <- try(.Internal(mtlrpcstats(TRUE)), silent = TRUE)

  f <- function(i) {
    nm <- as_string(sym(paste0("col_", i)))
    base <- vec_c(i + (1:8), i + (1:8))
    df <- tibble(x = i + (1:8), g = rep_len(c("a", "b"), 8))
    out <- df |>
      group_by(g) |>
      mutate(z = x * 2 + 1) |>
      summarise(s = sum(z), .groups = "drop") |>
      arrange(g) |>
      pull(s)
    list(
      s = out,
      nm = nm,
      vec_sum = sum(base)
    )
  }

  ref <- lapply(1:32, f)
  got <- mtlapply(1:32, f)
  stopifnot(isTRUE(all.equal(ref, got, tolerance = 0)))

  rpc <- try(.Internal(mtlrpcstats(FALSE)), silent = TRUE)
  if (!inherits(rpc, "try-error")) {
    cat("\nworker->main RPC stats:\n")
    print(rpc)
    stopifnot(unname(rpc[["calls.total"]]) >= 1L)
  }
}

cat("\ndplyr smoke ok\n")
