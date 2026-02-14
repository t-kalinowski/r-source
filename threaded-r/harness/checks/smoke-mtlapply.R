#!/usr/bin/env Rscript

stopifnot(exists("mtlapply", mode = "function"))

old <- getOption("threads")
on.exit(options(threads = old), add = TRUE)
options(threads = 4L)

x <- mtlapply(1:32, function(i) i + 1L)
stopifnot(identical(unlist(x, use.names = FALSE), 2:33))

err <- try(
  mtlapply(1:4, function(i) {
    assign("mtl_tmp_global", i, envir = globalenv())
    i
  }),
  silent = TRUE
)
stopifnot(inherits(err, "try-error"))
stopifnot(!exists("mtl_tmp_global", envir = globalenv(), inherits = FALSE))

cat("smoke-mtlapply ok\n")
