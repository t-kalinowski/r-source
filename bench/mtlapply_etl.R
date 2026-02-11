## "Real-ish" ETL/feature-engineering benchmark for experimental mtlapply().
##
## This benchmark is intentionally self-contained (base R only) and designed to
## look like a common workflow:
## - shard rows
## - mutate/feature engineer per shard (vectorized numeric work + allocations)
## - group-wise summarize
## - compute a small top-k per shard
## - reduce results back on the main thread
##
## Usage:
##   ./build-mtl/bin/R --vanilla -q -f bench/mtlapply_etl.R
##
## Controls:
## - MTL_ETL_N: number of rows, default 8000000
## - MTL_ETL_SHARDS: number of shards/tasks, default 64
## - MTL_ETL_GROUPS: number of groups, default 4096
## - MTL_ETL_ITERS: timing iterations, default 3
## - MTL_ETL_WARMUP: warmup iterations, default 1
## - MTL_ETL_THREADS: comma-separated thread counts, default "1,2,4,8"
## - MTL_ETL_FEAT_LOOPS: number of feature loops, default 4

stopifnot(exists("mtlapply"))

parse_int <- function(x, default) {
    if (!nzchar(x)) return(default)
    as.integer(x)
}

parse_int_vec <- function(x, default) {
    if (!nzchar(x)) return(default)
    as.integer(strsplit(x, ",", fixed = TRUE)[[1L]])
}

N <- parse_int(Sys.getenv("MTL_ETL_N"), 8000000L)
shards <- parse_int(Sys.getenv("MTL_ETL_SHARDS"), 64L)
ngroups <- parse_int(Sys.getenv("MTL_ETL_GROUPS"), 4096L)
iters <- parse_int(Sys.getenv("MTL_ETL_ITERS"), 3L)
warmup <- parse_int(Sys.getenv("MTL_ETL_WARMUP"), 1L)
threads <- parse_int_vec(Sys.getenv("MTL_ETL_THREADS"), c(1L, 2L, 4L, 8L))
feat_loops <- parse_int(Sys.getenv("MTL_ETL_FEAT_LOOPS"), 4L)

stopifnot(N >= 1L, shards >= 1L, ngroups >= 1L, iters >= 1L, warmup >= 0L, feat_loops >= 1L)
stopifnot(all(is.finite(threads)), all(threads >= 1L))

cat("mtlapply_etl settings:\n")
cat("N=", N,
    " shards=", shards,
    " groups=", ngroups,
    " feat_loops=", feat_loops,
    " iters=", iters,
    " warmup=", warmup,
    " threads=", paste(threads, collapse = ","),
    "\n", sep = "")

## Data generation: deterministic, avoids RNG and strings.
## Keep these in the main/global heap so workers can read them without copying.
x <- (as.double(seq_len(N) %% 1000L) - 500) / 10
y <- (as.double((seq_len(N) * 17L) %% 1000L) - 500) / 10
w <- (as.double((seq_len(N) * 31L) %% 1000L) + 1) / 1000
grp <- rep_len(seq_len(ngroups), N)

## Shards as index vectors: avoids split()/factor overhead.
idxs <- lapply(seq_len(shards), function(k) seq.int(k, N, by = shards))

process_shard <- function(idx) {
    ## "mutate" step: feature engineering.
    z <- x[idx]
    yy <- y[idx]
    ww <- w[idx]
    for (i in seq_len(feat_loops)) {
        ## Vectorized math + allocations (realistic for feature pipelines).
        z <- log1p(abs(z)) + sin(yy + z) * ww + cos(z - yy)
    }

    g <- grp[idx]

    ## "summarise" step: group-wise sum (all groups present by construction).
    ## Avoid S3 dispatch machinery in worker threads for now.
    s1 <- rowsum.default(z, g, reorder = FALSE)
    cnt <- tabulate(g, ngroups)

    ## "slice_max" step: top-k within shard (small return payload).
    ## Use quicksort explicitly to avoid radix-sort thread-local initialization.
    top <- sort.int(z, decreasing = TRUE, method = "quick")[seq_len(100L)]

    list(s1 = as.double(s1), cnt = cnt, top = top)
}

reduce_results <- function(res) {
    ## Reduce on main thread. Keep this small but non-trivial.
    s1 <- Reduce(`+`, lapply(res, `[[`, "s1"))
    cnt <- Reduce(`+`, lapply(res, `[[`, "cnt"))
    mu <- s1 / cnt
    top_all <- sort.int(unlist(lapply(res, `[[`, "top"), use.names = FALSE),
                       decreasing = TRUE, method = "quick")[seq_len(100L)]
    list(mu = mu, top = top_all)
}

time_elapsed <- function(expr, env) unname(system.time(eval(expr, env))[["elapsed"]])

summarize <- function(times) c(min = min(times), median = median(times), mean = mean(times), max = max(times))

run_method <- function(label, expr) {
    invisible(gc())
    exprq <- substitute(expr)
    if (warmup > 0L) for (i in seq_len(warmup)) invisible(eval(exprq, parent.frame()))
    ts <- numeric(iters)
    for (i in seq_len(iters)) ts[[i]] <- time_elapsed(exprq, parent.frame())
    invisible(gc())
    s <- summarize(ts)
    cat(sprintf("%-14s  median=%8.3f  mean=%8.3f  min=%8.3f  max=%8.3f\n",
                label, s[["median"]], s[["mean"]], s[["min"]], s[["max"]]))
    list(times = ts, summary = s)
}

## Correctness check once (subset).
idxs_check <- idxs[seq_len(min(4L, length(idxs)))]
ref <- reduce_results(lapply(idxs_check, process_shard))
cur <- reduce_results(mtlapply(idxs_check, process_shard, threads = max(threads)))
stopifnot(isTRUE(all.equal(ref, cur, tolerance = 0)))

cat("\n== ETL pipeline: shard -> mutate -> summarise -> top-k -> reduce ==\n")

base <- run_method("lapply", reduce_results(lapply(idxs, process_shard)))

for (t in threads) {
    m <- run_method(sprintf("mtlapply(%d)", t),
                    reduce_results(mtlapply(idxs, process_shard, threads = t)))
    bmed <- base$summary[["median"]]
    mmed <- m$summary[["median"]]
    sp <- if (bmed > 0 && mmed > 0) bmed / mmed else NA_real_
    cat(sprintf("%-14s  speedup=%8s (vs lapply median)\n",
                "", if (is.na(sp)) "NA" else sprintf("%.2fx", sp)))
}

cat("\nDone.\n")
