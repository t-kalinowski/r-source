#!/usr/bin/env Rscript

suppressPackageStartupMessages({
  library(bench)
})

stopifnot(exists("mtlapply"), exists("background"), exists("wait"))

old <- options(threads = 8L)
on.exit(options(old), add = TRUE)

cat("\n=== Demo 1: mtlapply scaling (matrix multiply workload) ===\n")
A <- matrix(runif(10000), 100, 100)
B <- matrix(runif(10000), 100, 100)
X <- 1:10000

task <- function(i) A %*% B + i

res_apply <- bench::mark(
  lapply = lapply(X, task),
  mtlapply_1 = { options(threads = 1L); mtlapply(X, task) },
  mtlapply_2 = { options(threads = 2L); mtlapply(X, task) },
  mtlapply_4 = { options(threads = 4L); mtlapply(X, task) },
  mtlapply_8 = { options(threads = 8L); mtlapply(X, task) },
  iterations = 5,
  check = FALSE
)
print(res_apply[, c("expression", "min", "median", "itr/sec", "mem_alloc")])
med_apply_sec <- as.numeric(res_apply$median) / 1e9
barplot(
  med_apply_sec,
  names.arg = as.character(res_apply$expression),
  las = 2,
  ylab = "Median elapsed (s)",
  main = "mtlapply scaling"
)

cat("\n=== Demo 2: background/wait next-ready queue ===\n")
work <- function(n) {
  x <- seq_len(n)
  sum(sin(x) * cos(x))
}
jobs <- sample(80000:180000, 16, replace = TRUE)

run_serial <- function() lapply(jobs, work)
run_parallel <- function() {
  options(threads = 8L)
  futs <- lapply(jobs, function(n) background(work(n)))
  pending <- Map(function(id, fut) list(id = id, fut = fut), seq_along(futs), futs)
  out <- vector("list", length(futs))
  while (length(pending)) {
    done <- wait(lapply(pending, `[[`, "fut"))
    i <- attr(done, "index")
    id <- pending[[i]]$id
    out[[id]] <- done$value
    pending <- pending[-i]
  }
  out
}

res_bg <- bench::mark(
  serial = run_serial(),
  background_wait = run_parallel(),
  iterations = 5,
  check = FALSE
)
print(res_bg[, c("expression", "min", "median", "itr/sec", "mem_alloc")])
med_bg_sec <- as.numeric(res_bg$median) / 1e9
barplot(
  med_bg_sec,
  names.arg = as.character(res_bg$expression),
  las = 2,
  ylab = "Median elapsed (s)",
  main = "background()/wait()"
)

cat("\nDone.\n")
