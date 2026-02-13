## Minimal repro smoke for threadpool notify FD wakeups via later::later_fd(readfds=...).
##
## This intentionally uses readfds-only (no timeout fallback) to detect missed
## wakeups in the notify->later path.
##
## Usage:
##   build-mtl-shlib/bin/R --vanilla -q -f tools/mtl-notify-readfd-smoke.R --args [rounds] [deadline_sec]

args <- commandArgs(trailingOnly = TRUE)
rounds <- if (length(args) >= 1L) as.integer(args[[1L]]) else 20L
deadline_sec <- if (length(args) >= 2L) as.numeric(args[[2L]]) else 0.5

if (!is.finite(rounds) || rounds < 1L) stop("invalid rounds", call. = FALSE)
if (!is.finite(deadline_sec) || deadline_sec <= 0) stop("invalid deadline_sec", call. = FALSE)
if (!requireNamespace("later", quietly = TRUE)) stop("later is required", call. = FALSE)
if (!exists("background", mode = "function") ||
    !exists("wait", mode = "function") ||
    !exists(".mt_notify_fd", envir = baseenv(), inherits = FALSE) ||
    !exists(".mt_notify_drain", envir = baseenv(), inherits = FALSE)) {
  stop("background()/wait()/base::.mt_notify_* are required", call. = FALSE)
}
notify_stats <- function(reset = FALSE) .Internal(mtnotifystats(as.logical(reset)))
has_notify_stats <- !inherits(try(notify_stats(FALSE), silent = TRUE), "try-error")

options(threads = max(2L, as.integer(getOption("threads", 2L))))
if (has_notify_stats) invisible(notify_stats(TRUE))

run_once <- function(i, deadline) {
  fd <- base::.mt_notify_fd()
  stopifnot(is.integer(fd), length(fd) == 1L, !is.na(fd), fd >= 0L)

  fired <- FALSE
  dereg <- later::later_fd(function(...) {
    base::.mt_notify_drain()
    fired <<- TRUE
  }, readfds = fd)
  on.exit(try(dereg(), silent = TRUE), add = TRUE)

  fut <- background(i + 1L)

  t0 <- proc.time()[["elapsed"]]
  while (!fired && (proc.time()[["elapsed"]] - t0) < deadline) {
    later::run_now(0.005)
  }

  got <- wait(fut)
  value_ok <- isTRUE(attr(got, "ok")) && identical(got$value, i + 1L)
  list(fired = fired, value_ok = value_ok)
}

fails <- integer(0)
fail_details <- list()
for (i in seq_len(rounds)) {
  res <- run_once(i, deadline_sec)
  if (!isTRUE(res$fired) || !isTRUE(res$value_ok)) {
    fails <- c(fails, i)
    if (has_notify_stats) {
      fail_details[[as.character(i)]] <- notify_stats(FALSE)
    }
  }
}

cat(sprintf("rounds=%d deadline=%.3fs failures=%d\n", rounds, deadline_sec, length(fails)))
if (has_notify_stats) {
  cat("notify stats:\n")
  print(notify_stats(FALSE))
}
if (length(fails)) {
  cat("failed rounds:", paste(fails, collapse = ", "), "\n")
  if (length(fail_details)) {
    cat("notify stats snapshots at failure:\n")
    print(fail_details)
  }
  stop("notify readfd smoke failed (missed callback wake)", call. = FALSE)
}
cat("notify readfd smoke ok\n")
