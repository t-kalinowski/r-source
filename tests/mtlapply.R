## Regression test for experimental mtlapply()
##
## This is intended to catch races in global runtime flags (notably GC enable)
## when evaluating in parallel worker interpreters.

stopifnot(exists("mtlapply"))

mtlapply_with_threads <- function(n, X, FUN, ...)
{
  old <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old), add = TRUE)
  options(mtlapply.threads = as.integer(n))
  mtlapply(X, FUN, ...)
}

with_mtl_threads <- function(n, expr)
{
  old <- getOption("mtlapply.threads")
  on.exit(options(mtlapply.threads = old), add = TRUE)
  options(mtlapply.threads = as.integer(n))
  force(expr)
}

# Workload should allocate and also touch global caches (symbols/CHARSXPs).
f <- function(i) {
  s <- paste0("sym-", i, "-", i)
  sym <- as.name(s)
  v <- cos(seq_len(i))
  list(s = s, sym = sym, v = v)
}

x <- mtlapply_with_threads(4L, 1:100, f)
stopifnot(length(x) == 100L)
stopifnot(isTRUE(all.equal(x[[3]]$v, cos(1:3))))

# Package/native-code story: install a minimal package with .Call() and ensure
# calls from mtlapply() workers work, while writes to globalenv() error.
pkg_src <- file.path(Sys.getenv("SRCDIR"), "Pkgs", "mtlPkg")
if (dir.exists(pkg_src)) {
  lib <- tempfile("mtlLib-")
  dir.create(lib)
  utils::install.packages(pkg_src, lib = lib, repos = NULL, type = "source", quiet = TRUE)
  stopifnot(requireNamespace("mtlPkg", lib.loc = lib, quietly = TRUE))

  vals <- mtlapply_with_threads(2L, 1:100, function(i) mtlPkg::mtl_add(i, i + 1))
  stopifnot(identical(unlist(vals, use.names = FALSE), as.double(1:100 + (1:100 + 1))))

  if (exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))
    rm(mtl_test_var, envir = .GlobalEnv)
  err_global_call <- try(mtlapply_with_threads(2L, 1:8, function(i) mtlPkg::mtl_define_global(i)),
                         silent = TRUE)
  stopifnot(inherits(err_global_call, "try-error"))
  stopifnot(!exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))
}

# Pure R writes to globalenv() from workers should also error.
if (exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))
  rm(mtl_test_var, envir = .GlobalEnv)
err_global_assign <- try(mtlapply_with_threads(2L, 1:8, function(i) {
  assign("mtl_test_var", i, envir = globalenv())
  i
}), silent = TRUE)
stopifnot(inherits(err_global_assign, "try-error"))
stopifnot(!exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))

err_global_eval <- try(mtlapply_with_threads(2L, 1:8, function(i) {
  eval(quote(mtl_test_var <- i), envir = globalenv())
  i
}), silent = TRUE)
stopifnot(inherits(err_global_eval, "try-error"))
stopifnot(!exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))

# options() writes in workers are local to the worker/job.
digits0 <- getOption("digits")
digits_vals <- mtlapply_with_threads(2L, 1:3, function(i) {
  options(digits = 7L + i)
  getOption("digits")
})
stopifnot(identical(as.integer(unlist(digits_vals, use.names = FALSE)), c(8L, 9L, 10L)))
stopifnot(identical(getOption("digits"), digits0))

# Worker threads must not change thread-count options.
err_opt_mtl <- try(mtlapply_with_threads(2L, 1:3, function(i) {
  options(mtlapply.threads = 8L)
  i
}), silent = TRUE)
stopifnot(inherits(err_opt_mtl, "try-error"))

err_opt_threads <- try(mtlapply_with_threads(2L, 1:3, function(i) {
  options(threads = 8L)
  i
}), silent = TRUE)
stopifnot(inherits(err_opt_threads, "try-error"))

# Nested mtlapply() should preserve closure-captured values.
outer_off <- 100L
nested <- mtlapply_with_threads(4L, 1:20, function(i) {
  inner_off <- i * 3L
  unlist(mtlapply(1:5, function(j) outer_off + inner_off + j), use.names = FALSE)
})
stopifnot(identical(nested[[4]], outer_off + 12L + 1:5))

# Nested workload should speed up with more worker threads.
ncores <- parallel::detectCores(logical = FALSE)
if (!is.na(ncores) && ncores >= 2L) {
  nested_work <- function() {
    mtlapply(1:64, function(i) {
      sum(unlist(mtlapply(1:4, function(j) sum(cos(seq_len(2000L + i + j)))),
                 use.names = FALSE))
    })
  }
  run_nested <- function(nthr) {
    median(replicate(3L, system.time(with_mtl_threads(nthr, nested_work()))[["elapsed"]]))
  }
  t1 <- run_nested(1L)
  t2 <- run_nested(2L)
  stopifnot(is.finite(t1), is.finite(t2), t2 < 0.95 * t1)
}

# Ensure we got actual overlap in worker evaluation.
invisible(.Internal(mtlparallelmax()))
invisible(mtlapply_with_threads(4L, rep(20000L, 8L), function(i) cos(seq_len(i))))
m <- .Internal(mtlparallelmax())
stopifnot(m >= 2L)

# The critical regression: a GC after mtlapply() must not wedge the interpreter.
invisible(gc())

# Error-path regression: an mtlapply() failure must not poison subsequent
# main-thread evaluation or error handling.
err <- try(mtlapply_with_threads(4L, 1:64, function(i) {
  if (i == 17L) stop("boom from worker")
  i
}), silent = TRUE)
stopifnot(inherits(err, "try-error"))

# Main-thread errors after mtlapply() should remain recoverable.
bad <- try(system.time(lapply(1:10, function(i) i, threads = 8L)), silent = TRUE)
stopifnot(inherits(bad, "try-error"))
ok <- lapply(1:100, function(i) i + 1L)
stopifnot(identical(unlist(ok, use.names = FALSE), 2:101))

# User-handled worker errors should behave like lapply().
handled_ref <- lapply(1:40, function(i) {
  tryCatch({
    if (i %% 9L == 0L) stop("boom")
    i
  }, error = function(e) -i)
})
handled_mtl <- mtlapply_with_threads(4L, 1:40, function(i) {
  tryCatch({
    if (i %% 9L == 0L) stop("boom")
    i
  }, error = function(e) -i)
})
stopifnot(identical(handled_ref, handled_mtl))

# Repeated worker failures should not poison later runs.
for (k in 1:10) {
  errk <- try(mtlapply_with_threads(4L, 1:128, function(i) {
    if (i == 37L) stop("boom")
    i
  }), silent = TRUE)
  stopifnot(inherits(errk, "try-error"))
}
ok2 <- mtlapply_with_threads(4L, 1:50, function(i) i + 3L)
stopifnot(identical(unlist(ok2, use.names = FALSE), 4:53))

cat("mtlapply ok\n")
