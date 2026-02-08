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
stopifnot(grepl("setting options is not supported in worker threads", conditionMessage(attr(e, "condition"))))

