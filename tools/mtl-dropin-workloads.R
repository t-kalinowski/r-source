## Varied "drop-in replacement" workloads for correctness + timing.
##
## Usage:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dropin-workloads.R --args \
##     [out_csv] [threads_csv] [iters]
##
## Example:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-dropin-workloads.R --args \
##     bench/results/dropin_workloads_latest.csv 1,2,4,8 3
##
## Notes:
## - Base-R only.
## - If mtlapply() is present, each workload is validated against lapply().
## - Includes explicit guard checks for worker global-state mutation errors.

parse_int <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(x)
}

parse_int_vec <- function(x, default) {
  if (!nzchar(x)) return(default)
  as.integer(strsplit(x, ",", fixed = TRUE)[[1L]])
}

time_median <- function(expr, iters) {
  exprq <- substitute(expr)
  ts <- numeric(iters)
  for (i in seq_len(iters)) {
    invisible(gc())
    ts[[i]] <- unname(system.time(eval(exprq, parent.frame()))[["elapsed"]])
  }
  median(ts)
}

with_mtl_threads <- function(n, expr) {
  old <- getOption("threads")
  on.exit(options(threads = old), add = TRUE)
  options(threads = as.integer(n))
  force(expr)
}

args <- commandArgs(trailingOnly = TRUE)
out_csv <- if (length(args) >= 1L) args[[1L]] else "bench/results/dropin_workloads_latest.csv"
threads <- parse_int_vec(if (length(args) >= 2L) args[[2L]] else "", c(1L, 2L, 4L, 8L))
iters <- parse_int(if (length(args) >= 3L) args[[3L]] else "", 3L)

stopifnot(length(threads) >= 1L, all(is.finite(threads)), all(threads >= 1L))
stopifnot(is.finite(iters), iters >= 1L)

has_mtlapply <- exists("mtlapply", mode = "function")

## Shared deterministic fixtures.
vec_cap <- as.double(seq_len(1024L)) / 10
strings_fixture <- sprintf("item-%04d", seq_len(256L))
ser_fixture <- list(
  ints = as.integer(seq_len(128L)),
  nums = as.double(seq_len(128L)) / 3,
  chars = strings_fixture[1:64]
)

workloads <- list(
  list(
    name = "closure_capture",
    x = seq_len(128L),
    fun = function(i) {
      j <- ((i - 1L) %% length(vec_cap)) + 1L
      vec_cap[[j]] + i
    }
  ),
  list(
    name = "promise_force",
    x = seq_len(128L),
    fun = function(i) {
      delayedAssign("z", i * 3L + 1L)
      z + 2L
    }
  ),
  list(
    name = "trycatch_error_path",
    x = seq_len(256L),
    fun = function(i) {
      tryCatch({
        if (i %% 19L == 0L) stop("boom")
        i * i
      }, error = function(e) -i)
    }
  ),
  list(
    name = "warning_suppressed",
    x = seq_len(256L),
    fun = function(i) {
      suppressWarnings({
        if (i %% 23L == 0L) warning("note")
        sqrt(i + 1L)
      })
    }
  ),
  list(
    name = "tempfile_read_write",
    x = seq_len(64L),
    fun = function(i) {
      path <- tempfile(fileext = ".txt")
      txt <- sprintf("row-%04d", i)
      writeLines(txt, path, useBytes = TRUE)
      got <- readLines(path, n = 1L, warn = FALSE)
      unlink(path)
      nchar(got, type = "bytes")
    }
  ),
  list(
    name = "regex_and_symbols",
    x = seq_len(128L),
    fun = function(i) {
      s <- strings_fixture[[((i - 1L) %% length(strings_fixture)) + 1L]]
      out <- gsub("[^0-9]", "", paste0("k-", s, "-", i))
      sym <- as.name(paste0("sym_", out))
      list(out = out, sym = sym)
    }
  ),
  list(
    name = "serialize_roundtrip",
    x = seq_len(96L),
    fun = function(i) {
      raw <- serialize(list(i = i, payload = ser_fixture), NULL, version = 3)
      obj <- unserialize(raw)
      obj$i + length(obj$payload$ints)
    }
  ),
  list(
    name = "small_lm_fit",
    x = seq_len(64L),
    fun = function(i) {
      n <- 128L
      x1 <- (seq_len(n) + i) / 10
      x2 <- sin(seq_len(n) + i / 5)
      y <- 1.5 + 2 * x1 - 0.8 * x2
      fit <- lm.fit(cbind(1, x1, x2), y)
      as.double(sum(fit$coefficients))
    }
  ),
  list(
    name = "nested_apply_compute",
    x = seq_len(48L),
    fun = function(i) {
      sum(unlist(lapply(1:8, function(j) sum(cos(seq_len(1024L) + i + j))),
                 use.names = FALSE))
    }
  ),
  list(
    name = "superassign_local_closure",
    x = seq_len(96L),
    fun = function(i) {
      acc <- 0L
      bump <- function() {
        acc <<- acc + i
        acc
      }
      c(bump(), bump(), acc)
    }
  )
)

rows <- list()
for (wl in workloads) {
  nm <- wl$name
  x <- wl$x
  fun <- wl$fun

  ref <- lapply(x, fun)
  l_med <- time_median(lapply(x, fun), iters)
  rows[[length(rows) + 1L]] <- data.frame(
    workload = nm,
    method = "lapply",
    threads = 0L,
    median_seconds = l_med,
    stringsAsFactors = FALSE
  )

  if (has_mtlapply) {
    for (t in threads) {
      got <- with_mtl_threads(t, mtlapply(x, fun))
      stopifnot(identical(ref, got))
      m_med <- time_median(with_mtl_threads(t, mtlapply(x, fun)), iters)
      rows[[length(rows) + 1L]] <- data.frame(
        workload = nm,
        method = "mtlapply",
        threads = as.integer(t),
        median_seconds = m_med,
        stringsAsFactors = FALSE
      )
    }
  }
}

if (has_mtlapply) {
  ## Explicit global-state mutation guards should fail cleanly and not poison session.
  if (exists("mtl_dropin_tmp", envir = .GlobalEnv, inherits = FALSE))
    rm(mtl_dropin_tmp, envir = .GlobalEnv)

  err_assign <- try(with_mtl_threads(max(threads), mtlapply(1:8, function(i) {
    assign("mtl_dropin_tmp", i, envir = globalenv())
    i
  })), silent = TRUE)
  stopifnot(inherits(err_assign, "try-error"))
  stopifnot(!exists("mtl_dropin_tmp", envir = .GlobalEnv, inherits = FALSE))

  err_setwd <- try(with_mtl_threads(max(threads), mtlapply(1:8, function(i) {
    setwd(tempdir())
    i
  })), silent = TRUE)
  stopifnot(inherits(err_setwd, "try-error"))

  ok_after <- lapply(1:20, function(i) i + 1L)
  stopifnot(identical(unlist(ok_after, use.names = FALSE), 2:21))
}

out <- do.call(rbind, rows)
dir.create(dirname(out_csv), recursive = TRUE, showWarnings = FALSE)
write.csv(out, out_csv, row.names = FALSE)

cat("wrote: ", out_csv, "\n", sep = "")

if (has_mtlapply) {
  ltab <- subset(out, method == "lapply", c("workload", "median_seconds"))
  names(ltab)[2] <- "lapply_s"
  mtab <- subset(out, method == "mtlapply", c("workload", "threads", "median_seconds"))
  names(mtab)[3] <- "mtlapply_s"
  s <- merge(mtab, ltab, by = "workload", all.x = TRUE, sort = TRUE)
  s$speedup <- s$lapply_s / s$mtlapply_s
  s$efficiency <- s$speedup / s$threads
  cat("\nsummary:\n")
  print(s[order(s$workload, s$threads), ], row.names = FALSE)
}

cat("\ndrop-in workload suite ok\n")
