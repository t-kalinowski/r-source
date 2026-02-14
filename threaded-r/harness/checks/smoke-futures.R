#!/usr/bin/env Rscript

stopifnot(exists("background", mode = "function"))
stopifnot(exists("wait", mode = "function"))
stopifnot(exists("cancel", mode = "function"))

old <- getOption("threads")
on.exit(options(threads = old), add = TRUE)
options(threads = 4L)

cpu_work <- function(n = 300000L) {
  s <- 0L
  for (i in seq_len(n)) s <- s + (i %% 7L)
  s
}

f1 <- background(cpu_work(250000L))
f2 <- background(cpu_work(300000L))
done1 <- wait(f1, timeout = 30)
done2 <- wait(f2, timeout = 30)
stopifnot(inherits(done1, "mt_future"), inherits(done2, "mt_future"))
stopifnot(isTRUE(attr(done1, "ok")), isTRUE(attr(done2, "ok")))
stopifnot(is.integer(done1$value), is.integer(done2$value))

f_cancel <- background(cpu_work(20000000L))
cancelled <- cancel(f_cancel)
stopifnot(is.logical(cancelled), length(cancelled) == 1L, !is.na(cancelled))

cat("smoke-futures ok\n")
