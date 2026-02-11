## Regression test for experimental mtlapply()
##
## This is intended to catch races in global runtime flags (notably GC enable)
## when evaluating in parallel worker interpreters.

stopifnot(exists("mtlapply"))

# Workload should allocate and also touch global caches (symbols/CHARSXPs).
f <- function(i) {
  s <- paste0("sym-", i, "-", i)
  sym <- as.name(s)
  v <- cos(seq_len(i))
  list(s = s, sym = sym, v = v)
}

x <- mtlapply(1:100, f, threads = 4L)
stopifnot(length(x) == 100L)
stopifnot(isTRUE(all.equal(x[[3]]$v, cos(1:3))))

# Package/native-code story: install a minimal package with .Call() and ensure
# calls from mtlapply() workers work and do not leak to the real global env.
pkg_src <- file.path(Sys.getenv("SRCDIR"), "Pkgs", "mtlPkg")
if (dir.exists(pkg_src)) {
  lib <- tempfile("mtlLib-")
  dir.create(lib)
  utils::install.packages(pkg_src, lib = lib, repos = NULL, type = "source", quiet = TRUE)
  stopifnot(requireNamespace("mtlPkg", lib.loc = lib, quietly = TRUE))

  vals <- mtlapply(1:100, function(i) mtlPkg::mtl_add(i, i + 1), threads = 2L)
  stopifnot(identical(unlist(vals, use.names = FALSE), as.double(1:100 + (1:100 + 1))))

  if (exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))
    rm(mtl_test_var, envir = .GlobalEnv)
  invisible(mtlapply(1:8, function(i) mtlPkg::mtl_define_global(i), threads = 2L))
  stopifnot(!exists("mtl_test_var", envir = .GlobalEnv, inherits = FALSE))
}

# Ensure we got actual overlap in worker evaluation.
invisible(.Internal(mtlparallelmax()))
invisible(mtlapply(rep(20000L, 8L), function(i) cos(seq_len(i)), threads = 4L))
m <- .Internal(mtlparallelmax())
stopifnot(m >= 2L)

# The critical regression: a GC after mtlapply() must not wedge the interpreter.
invisible(gc())

# Error-path regression: an mtlapply() failure must not poison subsequent
# main-thread evaluation or error handling.
err <- try(mtlapply(1:64, function(i) {
  if (i == 17L) stop("boom from worker")
  i
}, threads = 4L), silent = TRUE)
stopifnot(inherits(err, "try-error"))

# Main-thread errors after mtlapply() should remain recoverable.
bad <- try(system.time(lapply(1:10, function(i) i, threads = 8L)), silent = TRUE)
stopifnot(inherits(bad, "try-error"))
ok <- lapply(1:100, function(i) i + 1L)
stopifnot(identical(unlist(ok, use.names = FALSE), 2:101))

cat("mtlapply ok\n")
