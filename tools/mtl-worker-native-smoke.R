## Smoke test: exercise common package native-code paths from worker threads.
##
## Usage:
##   build-mtl/bin/R --vanilla -q -f tools/mtl-worker-native-smoke.R --args \
##     /Users/tomasz/Library/R/arm64/4.6/library [threads] [n]

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1L || length(args) > 3L)
  stop("usage: <libpath> [threads] [n]", call. = FALSE)

lib <- args[[1L]]
threads <- if (length(args) >= 2L) as.integer(args[[2L]]) else 4L
n <- if (length(args) >= 3L) as.integer(args[[3L]]) else 64L

if (!is.finite(threads) || threads < 1L) stop("invalid threads", call. = FALSE)
if (!is.finite(n) || n < 1L) stop("invalid n", call. = FALSE)
if (!dir.exists(lib)) stop("library path does not exist: ", lib, call. = FALSE)

.libPaths(c(lib, .libPaths()))
cat("libPaths:\n", paste(.libPaths(), collapse = "\n"), "\n", sep = "")

suppressPackageStartupMessages(library(digest))
suppressPackageStartupMessages(library(rlang))
suppressPackageStartupMessages(library(vctrs))

if (!exists("mtlapply")) {
  cat("\nmtlapply not available; skipping worker-native smoke\n")
  quit(status = 0L)
}

old_threads <- getOption("mtlapply.threads")
on.exit(options(mtlapply.threads = old_threads), add = TRUE)
options(mtlapply.threads = threads)

f <- function(i) {
  payload <- paste0("payload-", i)
  h <- digest(payload, algo = "xxhash64", serialize = FALSE)
  nm <- as_string(sym(paste0("col_", i)))
  vv <- vec_c(i, i + 1L, i + 2L, i + 3L)
  m <- vec_c(vv, vv)
  s <- vec_slice(m, c(1L, length(m)))
  list(
    hash = h,
    nm = nm,
    vec_sum = sum(vv),
    m_sum = sum(m),
    edge = as.integer(s)
  )
}

## Compare serial vs worker results.
ref <- lapply(seq_len(n), f)
got <- mtlapply(seq_len(n), f)
stopifnot(isTRUE(all.equal(ref, got, tolerance = 0)))

## Run a short stress loop to exercise repeated worker dispatch of package code.
for (k in 1:3) {
  got2 <- mtlapply(seq_len(n), f)
  stopifnot(identical(got, got2))
}

rpc <- try(.Internal(mtlrpcstats(FALSE)), silent = TRUE)
if (!inherits(rpc, "try-error")) {
  cat("\nworker->main RPC stats (post worker-native smoke):\n")
  print(rpc)
}

cat("\nworker-native package smoke ok\n")
