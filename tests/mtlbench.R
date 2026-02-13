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
## - MTLBENCH_IO_FILES: number of files for I/O benchmark, default NTASKS
## - MTLBENCH_IO_SIZE_KB: per-file size in KiB for I/O benchmark, default 2048
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
io_files <- parse_int(Sys.getenv("MTLBENCH_IO_FILES"), ntasks)
io_size_kb <- parse_int(Sys.getenv("MTLBENCH_IO_SIZE_KB"), 2048L)
include_call <- identical(Sys.getenv("MTLBENCH_CALL"), "1")
nested_outer <- parse_int(Sys.getenv("MTLBENCH_NEST_OUTER"), 64L)
nested_mid <- parse_int(Sys.getenv("MTLBENCH_NEST_MID"), 8L)
nested_inner <- parse_int(Sys.getenv("MTLBENCH_NEST_INNER"), 8L)
nested_work_n <- parse_int(Sys.getenv("MTLBENCH_NEST_WORK_N"), 4096L)
nested_work_reps <- parse_int(Sys.getenv("MTLBENCH_NEST_WORK_REPS"), 8L)

stopifnot(all(is.finite(threads)))
stopifnot(iters >= 1L, warmup >= 0L, ntasks >= 1L, size >= 1L, loop_n >= 1L, str_iter >= 1L)
stopifnot(io_files >= 1L, io_size_kb >= 1L)
stopifnot(nested_outer >= 1L, nested_mid >= 1L, nested_inner >= 1L)
stopifnot(nested_work_n >= 1L, nested_work_reps >= 1L)

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
    old <- getOption("threads")
    on.exit(options(threads = old), add = TRUE)
    options(threads = as.integer(n))
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
    " loop_n=", loop_n, " str_iter=", str_iter,
    " io_files=", io_files, " io_size_kb=", io_size_kb, "\n", sep = "")
cat("nested_outer=", nested_outer,
    " nested_mid=", nested_mid,
    " nested_inner=", nested_inner,
    " nested_work_n=", nested_work_n,
    " nested_work_reps=", nested_work_reps, "\n", sep = "")

mk_io_fixture <- function(nfiles, size_kb) {
    d <- tempfile("mtlbench-io-")
    dir.create(d)
    paths <- file.path(d, sprintf("io-%05d.bin", seq_len(nfiles)))
    need <- as.integer(size_kb) * 1024L
    payload <- charToRaw(paste(rep("mtlbench-io-0123456789abcdef", 128L), collapse = ""))
    payload_n <- length(payload)
    for (p in paths) {
        con <- file(p, "wb")
        remaining <- need
        while (remaining > 0L) {
            n <- min(remaining, payload_n)
            if (n == payload_n) {
                writeBin(payload, con, useBytes = TRUE)
            } else {
                writeBin(payload[seq_len(n)], con, useBytes = TRUE)
            }
            remaining <- remaining - n
        }
        close(con)
    }
    list(dir = d, paths = paths)
}

io_fixture <- mk_io_fixture(io_files, io_size_kb)

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
        name = "filesystem I/O (md5sum over independent files)",
        x = io_fixture$paths,
        fun = function(path) {
            unname(tools::md5sum(path))
        }
    ),
    list(
        name = "pure R loop (interpreter bound, no big alloc)",
        x = rep.int(loop_n, ntasks),
        fun = function(n) {
            sum(seq_len(n) %% 97L)
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

bench_background_io <- function(paths, threads, iters, warmup) {
    cat("\n== background()/wait() queue benchmark (filesystem I/O) ==\n")

    io_fun <- function(path) unname(tools::md5sum(path))

    run_background_wait <- function(x) {
        futs <- lapply(x, function(path) background(io_fun(path)))
        pending <- futs
        idx_map <- seq_along(futs)
        out <- vector("list", length(futs))
        while (length(pending)) {
            got <- wait(pending)
            k <- attr(got, "index")
            out[[idx_map[[k]]]] <- got$value
            pending <- pending[-k]
            idx_map <- idx_map[-k]
        }
        out
    }

    x_check <- paths[seq_len(min(8L, length(paths)))]
    ref <- lapply(x_check, io_fun)
    cur <- with_mtl_threads(max(threads), run_background_wait(x_check))
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
        s
    }

    base <- run_method("lapply", lapply(paths, io_fun))
    for (t in threads) {
        s_mtl <- run_method(sprintf("mtlapply(%d)", t),
                            with_mtl_threads(t, mtlapply(paths, io_fun)))
        sp_mtl <- if (base[["median"]] > 0 && s_mtl[["median"]] > 0) base[["median"]] / s_mtl[["median"]] else NA_real_
        cat(sprintf("%-18s  speedup=%8s (vs lapply median)\n",
                    "", if (is.na(sp_mtl)) "NA" else sprintf("%.2fx", sp_mtl)))

        s_bg <- run_method(sprintf("bg+wait(%d)", t),
                           with_mtl_threads(t, run_background_wait(paths)))
        sp_bg <- if (base[["median"]] > 0 && s_bg[["median"]] > 0) base[["median"]] / s_bg[["median"]] else NA_real_
        cat(sprintf("%-18s  speedup=%8s (vs lapply median)\n",
                    "", if (is.na(sp_bg)) "NA" else sprintf("%.2fx", sp_bg)))
    }
}

bench_background_io(io_fixture$paths, threads, iters, warmup)

bench_nested_case <- function(threads, iters, warmup, outer_n, mid_n, inner_n, work_n, work_reps) {
    cat("\n== nested 3-level apply: lapply/lapply/lapply vs mtlapply/mtlapply/mtlapply ==\n")

    do_work <- function(seed) {
        base <- as.double(seed)
        acc <- 0
        for (r in seq_len(work_reps)) {
            acc <- acc + sum(cos(seq_len(work_n) + base + r))
        }
        acc
    }

    mk_outer <- function() {
        out <- vector("list", outer_n)
        v <- seq_len(inner_n)
        for (i in seq_len(outer_n)) {
            mid <- vector("list", mid_n)
            for (j in seq_len(mid_n))
                mid[[j]] <- v + (i * 17L + j * 31L)
            out[[i]] <- mid
        }
        out
    }

    x <- mk_outer()

    nested_lapply <- function(xx) {
        lapply(xx, function(a) lapply(a, function(b) lapply(b, do_work)))
    }

    nested_mtlapply <- function(xx) {
        mtlapply(xx, function(a) mtlapply(a, function(b) mtlapply(b, do_work)))
    }

    ref <- nested_lapply(x[seq_len(min(4L, length(x)))])
    cur <- with_mtl_threads(max(threads), nested_mtlapply(x[seq_len(min(4L, length(x)))]))
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
        s
    }

    base <- run_method("lapply^3", nested_lapply(x))
    for (t in threads) {
        s <- run_method(sprintf("mtlapply^3(%d)", t),
                        with_mtl_threads(t, nested_mtlapply(x)))
        sp <- if (base[["median"]] > 0 && s[["median"]] > 0) base[["median"]] / s[["median"]] else NA_real_
        cat(sprintf("%-18s  speedup=%8s (vs lapply^3 median)\n",
                    "", if (is.na(sp)) "NA" else sprintf("%.2fx", sp)))
    }
}

bench_nested_case(threads, iters, warmup,
                  nested_outer, nested_mid, nested_inner,
                  nested_work_n, nested_work_reps)

unlink(io_fixture$dir, recursive = TRUE, force = TRUE)

cat("\nDone.\n")
