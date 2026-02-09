## Tests for experimental mtlapply()

## Basic correctness.
x <- mtlapply(1:16, function(i) i * i, threads = 2L)
y <- lapply(1:16, function(i) i * i)
stopifnot(identical(x, y))

## Reading options from workers is allowed.
ow <- getOption("width")
w <- mtlapply(1:1, function(i) getOption("width"), threads = 2L)[[1]]
stopifnot(identical(w, ow))

## Setting options from workers is not supported.
e <- try(mtlapply(1:1, function(i) options(width = 80), threads = 2L), silent = TRUE)
stopifnot(inherits(e, "try-error"))
stopifnot(grepl("cannot set options from mtlapply\\(\\) worker threads", conditionMessage(attr(e, "condition"))))

## Superassignment is not allowed from workers.
g <- 0L
e_sup <- try(mtlapply(1:2, function(i) { g <<- i; i }, threads = 2L), silent = TRUE)
stopifnot(inherits(e_sup, "try-error"))
stopifnot(identical(g, 0L))
stopifnot(grepl("superassignment is not allowed", conditionMessage(attr(e_sup, "condition"))))

## tempfile() must be safe to call from workers.
tf <- unlist(mtlapply(1:20, function(i) tempfile(pattern = "mtl"), threads = 4L), use.names = FALSE)
stopifnot(length(unique(tf)) == length(tf))

## CHARSXP creation/interning must be safe from workers.
x_chr <- unlist(mtlapply(1:50, \(i) paste0("s", i), threads = 4L), use.names = FALSE)
y_chr <- unlist(lapply(1:50, \(i) paste0("s", i)), use.names = FALSE)
stopifnot(identical(x_chr, y_chr))

## Symbol interning must be safe from workers.
x_sym <- mtlapply(1:50, \(i) as.name(paste0("sym", i)), threads = 4L)
y_sym <- lapply(1:50, \(i) as.name(paste0("sym", i)))
stopifnot(identical(vapply(x_sym, as.character, ""), vapply(y_sym, as.character, "")))

## Weak references/finalizers are not supported from workers (yet).
e_fin <- try(mtlapply(1:1, \(i) { e <- new.env(); reg.finalizer(e, \(x) NULL); NULL }, threads = 2L), silent = TRUE)
stopifnot(inherits(e_fin, "try-error"))
stopifnot(grepl("weak references/finalizers are not supported", conditionMessage(attr(e_fin, "condition"))))

## Errors in workers should not leave shared internal locks in a stuck state.
e2 <- try(mtlapply(1:4, \(i) if (i == 2L) stop("boom") else i, threads = 2L), silent = TRUE)
stopifnot(inherits(e2, "try-error"))
z <- mtlapply(1:8, \(i) i + 1L, threads = 2L)
stopifnot(identical(z, lapply(1:8, \(i) i + 1L)))

## Ensure we get actual overlap in compute-heavy primitives.
## (mtlparallelmax() is an internal counter of max concurrent worker evals.)
invisible(.Internal(mtlparallelmax()))
invisible(mtlapply(rep(100000L, 8L), \(i) cos(seq(i)), threads = 4L))
m <- .Internal(mtlparallelmax())
stopifnot(m >= 2L)

## .Call() from worker threads is supported, but serialized under the global lock.
## Build a tiny shared library in a tempdir on the main thread, then call it from workers.
td <- tempfile("mtlcall-")
dir.create(td)
ofile <- file.path(td, "mtlcall.c")
writeLines(c(
  "#include <R.h>",
  "#include <Rinternals.h>",
  "",
  "SEXP mtlcall_alloc_int(SEXP x) {",
  "  SEXP ans = PROTECT(allocVector(INTSXP, 1));",
  "  INTEGER(ans)[0] = asInteger(x) + 1;",
  "  UNPROTECT(1);",
  "  return ans;",
  "}"
), ofile)
oldwd <- getwd()
setwd(td)
on.exit(setwd(oldwd), add = TRUE)
cmd <- file.path(R.home("bin"), "R")
out <- system2(cmd, c("CMD", "SHLIB", basename(ofile)), stdout = TRUE, stderr = TRUE)
st <- attr(out, "status")
if (!is.null(st) && st != 0)
    stop(paste(out, collapse = "\n"))
dynlib <- Sys.glob(paste0("mtlcall", .Platform$dynlib.ext))
stopifnot(length(dynlib) == 1)
dyn.load(dynlib)
x_call <- mtlapply(1:50, \(i) .Call("mtlcall_alloc_int", i), threads = 4L)
y_call <- lapply(1:50, \(i) .Call("mtlcall_alloc_int", i))
stopifnot(identical(x_call, y_call))
