## Smoke benchmark for quickr + mtlapply worker execution.
##
## Usage:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-quickr-smoke.R --args \
##     [libpath] [threads] [tasks] [max_ratio]
##
## Notes:
## - Package loading and compilation happen on the main thread.
## - Worker closures only execute already-compiled functions.

args <- commandArgs(trailingOnly = TRUE)
if (length(args) > 4L)
  stop("usage: [libpath] [threads] [tasks] [max_ratio]", call. = FALSE)

lib <- if (length(args) >= 1L && nzchar(args[[1L]])) args[[1L]] else ""
threads <- if (length(args) >= 2L) as.integer(args[[2L]]) else 4L
tasks <- if (length(args) >= 3L) as.integer(args[[3L]]) else 4L
max_ratio <- if (length(args) >= 4L) as.numeric(args[[4L]]) else 1.50

if (!is.na(lib) && nzchar(lib)) {
  if (!dir.exists(lib)) stop("library path does not exist: ", lib, call. = FALSE)
  .libPaths(c(lib, .libPaths()))
}

if (!is.finite(threads) || threads < 1L) stop("invalid threads", call. = FALSE)
if (!is.finite(tasks) || tasks < 1L) stop("invalid tasks", call. = FALSE)
if (!is.finite(max_ratio) || max_ratio <= 0) stop("invalid max_ratio", call. = FALSE)

if (!exists("mtlapply", mode = "function")) {
  cat("mtlapply not available; skipping quickr smoke\n")
  quit(status = 0L)
}

if (!requireNamespace("quickr", quietly = TRUE) ||
    !requireNamespace("inline", quietly = TRUE)) {
  cat("quickr/inline not installed; skipping quickr smoke\n")
  quit(status = 0L)
}

suppressPackageStartupMessages(library(quickr))
suppressPackageStartupMessages(library(inline))

slow_convolve <- function(a, b) {
  declare(type(a = double(NA)), type(b = double(NA)))
  ab <- double(length(a) + length(b) - 1)
  for (i in seq_along(a)) {
    for (j in seq_along(b)) {
      ab[i + j - 1] <- ab[i + j - 1] + a[i] * b[j]
    }
  }
  ab
}

quick_convolve <- quickr::quick(slow_convolve)

convolve_c <- inline::cfunction(
  sig = c(a = "SEXP", b = "SEXP"),
  body = r"({
    int na, nb, nab;
    double *xa, *xb, *xab;
    SEXP ab;
    a = PROTECT(Rf_coerceVector(a, REALSXP));
    b = PROTECT(Rf_coerceVector(b, REALSXP));
    na = Rf_length(a); nb = Rf_length(b); nab = na + nb - 1;
    ab = PROTECT(Rf_allocVector(REALSXP, nab));
    xa = REAL(a); xb = REAL(b); xab = REAL(ab);
    for (int i = 0; i < nab; i++) xab[i] = 0.0;
    for (int i = 0; i < na; i++)
      for (int j = 0; j < nb; j++)
        xab[i + j] += xa[i] * xb[j];
    UNPROTECT(3);
    return ab;
})"
)

make_inputs <- function(i, na = 40000L, nb = 100L) {
  list(
    a = ((seq_len(na) * (i + 17L)) %% 10000L) / 10000,
    b = ((seq_len(nb) * (i + 31L)) %% 10000L) / 10000
  )
}

do_work <- function(i) {
  inp <- make_inputs(i)
  r <- slow_convolve(inp$a, inp$b)
  q <- quick_convolve(inp$a, inp$b)
  c_out <- convolve_c(inp$a, inp$b)

  ## Keep the benchmark deterministic and verify equivalence in-worker.
  sr <- sum(r)
  sq <- sum(q)
  sc <- sum(c_out)
  if (!isTRUE(all.equal(sr, sq, tolerance = 1e-7)) ||
      !isTRUE(all.equal(sr, sc, tolerance = 1e-7))) {
    stop("quickr smoke: convolution variants diverged", call. = FALSE)
  }
  c(r = sr, quickr = sq, c = sc)
}

## Warm up compile/loading paths once on the main thread.
invisible(do_work(1L))

old_threads <- getOption("mtlapply.threads")
on.exit(options(mtlapply.threads = old_threads), add = TRUE)
options(mtlapply.threads = as.integer(threads))

serial <- lapply(seq_len(tasks), do_work)
threaded <- mtlapply(seq_len(tasks), do_work)
stopifnot(isTRUE(all.equal(serial, threaded, tolerance = 1e-7)))

serial_elapsed <- unname(system.time(lapply(seq_len(tasks), do_work))[["elapsed"]])
threaded_elapsed <- unname(system.time(mtlapply(seq_len(tasks), do_work))[["elapsed"]])
ratio <- threaded_elapsed / serial_elapsed

res <- data.frame(
  tasks = tasks,
  threads = threads,
  serial_elapsed_s = serial_elapsed,
  mtlapply_elapsed_s = threaded_elapsed,
  mtl_over_lapply = ratio,
  speedup = serial_elapsed / threaded_elapsed
)
print(res, row.names = FALSE, digits = 4)

if (ratio > max_ratio) {
  stop(sprintf(
    "quickr smoke performance regression: mtl/lapply ratio %.3f > %.3f",
    ratio, max_ratio
  ), call. = FALSE)
}

cat("quickr smoke ok\n")
