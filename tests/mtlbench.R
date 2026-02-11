## Micro-benchmarks for experimental mtlapply()
##
## This script is intentionally NOT part of `make check` (too noisy/slow).
##
## Usage (from repo root, using your built R):
##   /tmp/r-build-threads2/bin/R --vanilla -q -f tests/mtlbench.R
##
## Controls:
## - MTLBENCH_THREADS: comma-separated thread counts, default "1,2,4"
## - MTLBENCH_ITERS: timing iterations per method, default 3
## - MTLBENCH_WARMUP: warmup iterations per method, default 1
## - MTLBENCH_NTASKS: number of tasks per workload, default 16
## - MTLBENCH_SIZE: size knob for some workloads, default 2000
## - MTLBENCH_LOOPN: iterations for the pure-R loop workload, default 200000
## - MTLBENCH_STRITER: iterations for the string/symbol interning stress workload, default 5000
## - MTLBENCH_CALL: "1" to include an extra .Call() benchmark (needs a compiler)

stopifnot(exists("mtlapply"))

parse_int <- function(x, default) {
    if (!nzchar(x)) return(default)
    as.integer(x)
}

parse_int_vec <- function(x, default) {
    if (!nzchar(x)) return(default)
    as.integer(strsplit(x, ",", fixed = TRUE)[[1L]])
}

threads <- parse_int_vec(Sys.getenv("MTLBENCH_THREADS"), c(1L, 2L, 4L))
warmup <- parse_int(Sys.getenv("MTLBENCH_WARMUP"), 1L)
iters <- parse_int(Sys.getenv("MTLBENCH_ITERS"), 3L)
ntasks <- parse_int(Sys.getenv("MTLBENCH_NTASKS"), 16L)
size <- parse_int(Sys.getenv("MTLBENCH_SIZE"), 2000L)
loop_n <- parse_int(Sys.getenv("MTLBENCH_LOOPN"), 200000L)
str_iter <- parse_int(Sys.getenv("MTLBENCH_STRITER"), 5000L)
include_call <- identical(Sys.getenv("MTLBENCH_CALL"), "1")

stopifnot(all(is.finite(threads)))
stopifnot(iters >= 1L, warmup >= 0L, ntasks >= 1L, size >= 1L, loop_n >= 1L, str_iter >= 1L)

time_elapsed <- function(expr, env) {
    unname(system.time(eval(expr, env))[["elapsed"]])
}

summarize_times <- function(times) {
    c(
        n = length(times),
        min = min(times),
        median = median(times),
        mean = mean(times),
        max = max(times)
    )
}

with_mtl_threads <- function(n, expr)
{
    old <- getOption("mtlapply.threads")
    on.exit(options(mtlapply.threads = old), add = TRUE)
    options(mtlapply.threads = as.integer(n))
    force(expr)
}

bench_case <- function(name, x, fun, threads, iters, warmup) {
    cat("\n== ", name, " ==\n", sep = "")

    # Correctness check (once).
    x_check <- x[seq_len(min(8L, length(x)))]
    ref <- lapply(x_check, fun)
    cur <- with_mtl_threads(max(threads), mtlapply(x_check, fun))
    stopifnot(identical(ref, cur))

    run_method <- function(label, expr) {
        invisible(gc())
        exprq <- substitute(expr)
        if (warmup > 0L) {
            for (i in seq_len(warmup)) invisible(eval(exprq, parent.frame()))
        }
        ts <- numeric(iters)
        for (i in seq_len(iters)) ts[[i]] <- time_elapsed(exprq, parent.frame())
        invisible(gc())
        s <- summarize_times(ts)
        cat(sprintf("%-18s  median=%8.3f  mean=%8.3f  min=%8.3f  max=%8.3f\n",
                    label, s[["median"]], s[["mean"]], s[["min"]], s[["max"]]))
        list(times = ts, summary = s, label = label)
    }

    base <- run_method("lapply", lapply(x, fun))

    for (t in threads) {
        label <- sprintf("mtlapply(%d)", t)
        m <- run_method(label, with_mtl_threads(t, mtlapply(x, fun)))
        bmed <- base$summary[["median"]]
        mmed <- m$summary[["median"]]
        sp <- if (bmed > 0 && mmed > 0) bmed / mmed else NA_real_
        cat(sprintf("%-18s  speedup=%8s (vs lapply median)\n",
                    "", if (is.na(sp)) "NA" else sprintf("%.2fx", sp)))
    }

    invisible(base)
}

cat("mtlbench settings:\n")
cat("threads=", paste(threads, collapse = ","), "\n", sep = "")
cat("iters=", iters, " warmup=", warmup,
    " ntasks=", ntasks, " size=", size,
    " loop_n=", loop_n, " str_iter=", str_iter, "\n", sep = "")

cases <- list(
    list(
        name = "trivial scalar (overhead dominated)",
        x = seq_len(ntasks),
        fun = function(i) i * i
    ),
    list(
        name = "vectorized math cos(seq_len(n)) (compute + alloc)",
        x = rep.int(size, ntasks),
        fun = function(n) cos(seq_len(n))
    ),
    list(
        name = "string + symbol interning (paste0/as.name)",
        x = seq_len(ntasks),
        fun = function(i) {
            s <- paste0("sym", i, "-", i)
            list(s = s, sym = as.name(s))
        }
    ),
    list(
        name = "list building (alloc heavy, shallow)",
        x = rep.int(1000L, ntasks),
        fun = function(n) {
            # Avoid RNG; make deterministic content.
            v <- seq_len(n)
            list(v = v, w = v + 1L, m = matrix(v[1:100], 10, 10))
        }
    ),
    list(
        name = "pure R loop (interpreter bound, no big alloc)",
        x = rep.int(loop_n, ntasks),
        fun = function(n) {
            # Avoid allocating a big sequence by using a while loop.
            i <- 1L
            acc <- 0L
            while (i <= n) {
                acc <- acc + (i %% 97L)
                i <- i + 1L
            }
            acc
        }
    ),
    list(
        name = "string + symbol interning stress (paste0/as.name loop)",
        x = seq_len(ntasks),
        fun = function(i) {
            s <- ""
            sym <- NULL
            for (j in seq_len(str_iter)) {
                s <- paste0("sym", i, "-", j)
                sym <- as.name(s)
            }
            list(s = s, sym = sym)
        }
    )
)

if (include_call) {
    # One-off .Call benchmark to show serialization under the global lock.
    td <- tempfile("mtlbench-call-")
    dir.create(td)
    src <- file.path(td, "mtlbench_call.c")
    writeLines(c(
        "#include <R.h>",
        "#include <Rinternals.h>",
        "",
        "SEXP mtlbench_call(SEXP x) {",
        "  SEXP ans = PROTECT(allocVector(INTSXP, 1));",
        "  INTEGER(ans)[0] = asInteger(x) + 1;",
        "  UNPROTECT(1);",
        "  return ans;",
        "}"
    ), src)
    oldwd <- getwd()
    setwd(td)
    on.exit(setwd(oldwd), add = TRUE)
    out <- system2(file.path(R.home("bin"), "R"),
                   c("CMD", "SHLIB", basename(src)),
                   stdout = TRUE, stderr = TRUE)
    st <- attr(out, "status")
    if (!is.null(st) && st != 0)
        stop(paste(out, collapse = "\n"))
    dynlib <- Sys.glob(paste0("mtlbench_call", .Platform$dynlib.ext))
    stopifnot(length(dynlib) == 1L)
    dyn.load(dynlib)

    cases[[length(cases) + 1L]] <- list(
        name = ".Call alloc (serialized under global lock)",
        x = seq_len(ntasks),
        fun = function(i) .Call("mtlbench_call", i)
    )
}

for (c in cases) {
    bench_case(c$name, c$x, c$fun, threads, iters, warmup)
}

cat("\nDone.\n")
