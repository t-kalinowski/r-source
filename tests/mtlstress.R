## Stress tests for experimental mtlapply()
##
## This is intentionally NOT part of `make check`.
##
## Usage:
##   /tmp/r-build-threads2/bin/R --vanilla -q -f tests/mtlstress.R
##
## Controls:
## - MTLSTRESS_ITERS: number of iterations, default 25
## - MTLSTRESS_MAX_THREADS: max threads to pick, default 4
## - MTLSTRESS_MAX_TASKS: max tasks per iteration, default 32
## - MTLSTRESS_GC: "1" to call gc() inside workers (heavier), default "0"

stopifnot(exists("mtlapply"))

iters <- as.integer(Sys.getenv("MTLSTRESS_ITERS", "25"))
max_threads <- as.integer(Sys.getenv("MTLSTRESS_MAX_THREADS", "4"))
max_tasks <- as.integer(Sys.getenv("MTLSTRESS_MAX_TASKS", "32"))
do_gc <- identical(Sys.getenv("MTLSTRESS_GC", "0"), "1")

stopifnot(iters >= 1L, max_threads >= 1L, max_tasks >= 1L)

set.seed(1)

worker_fun <- function(i, k) {
    # Keep this deterministic and allocation-heavy enough to shake out races.
    s <- paste0("k", k, "-", "i", i)
    sym <- as.name(paste0("sym-", k, "-", i))
    v <- as.integer(i) + seq_len(100L)
    m <- matrix(v[1:100], 10, 10)
    if (do_gc) invisible(gc())
    list(s = s, sym = sym, v = v, m = m, sum = sum(v), norm = sqrt(sum(m * m)))
}

for (k in seq_len(iters)) {
    threads <- sample.int(max_threads, 1L)
    ntasks <- sample.int(max_tasks, 1L)
    x <- seq_len(ntasks)
    f <- function(i) worker_fun(i, k)
    options(mtlapply.threads = threads)

    a <- lapply(x, f)
    b <- mtlapply(x, f)
    if (!identical(a, b)) {
        stop(sprintf("mismatch at iter=%d threads=%d ntasks=%d", k, threads, ntasks))
    }

    if (k %% 5L == 0L) {
        cat("ok:", k, "iters\n")
    }
}

cat("stress ok\n")
