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

## tempfile() must be safe to call from workers (serializes via global lock).
tf <- unlist(mtlapply(1:20, function(i) tempfile(pattern = "mtl"), threads = 4L), use.names = FALSE)
stopifnot(length(unique(tf)) == length(tf))

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
