## Regression tests for experimental background()/wait()/cancel().

stopifnot(exists("background"), exists("wait"), exists("cancel"))

old_threads <- getOption("mtlapply.threads")
on.exit(options(mtlapply.threads = old_threads), add = TRUE)
options(mtlapply.threads = 4L)

## Basic wait-any semantics and value propagation.
futs <- lapply(1:40, function(i) background(i + 100L))
stopifnot(all(vapply(futs, function(f) identical(f$value, quote(.mt_unresolved)), logical(1))))

pending <- futs
vals <- integer(0)
while (length(pending)) {
    got <- wait(pending)
    stopifnot(inherits(got, "mt_future"))
    idx <- attr(got, "index")
    stopifnot(isTRUE(attr(got, "ok")))
    vals <- c(vals, got$value)
    pending <- pending[-idx]
}
stopifnot(identical(sort(vals), 101:140))

## timeout=0 should return NULL when no future has completed yet.
f_slow <- background({ Sys.sleep(0.2); 42L })
stopifnot(is.null(wait(list(f_slow), timeout = 0)))
stopifnot(inherits(try(wait(list(f_slow), timeout = -1), silent = TRUE), "try-error"))
stopifnot(identical(wait(f_slow)$value, 42L))

## Worker error should be surfaced in the returned future value.
f_err <- background(stop("boom from background"))
got_err <- wait(f_err)
stopifnot(inherits(got_err$value, "error"))
stopifnot(grepl("boom from background", conditionMessage(got_err$value), fixed = TRUE))

## Cancel should mark pending/running jobs as cancelled.
f_cancel <- background({ Sys.sleep(0.2); 99L })
stopifnot(isTRUE(cancel(f_cancel)))
got_cancel <- wait(f_cancel)
stopifnot(isTRUE(attr(got_cancel, "cancelled")))
stopifnot(inherits(got_cancel$value, "mt_cancelled"))

## A successful run after error/cancel should still work.
f_ok <- background(sum(seq_len(1000L)))
got_ok <- wait(f_ok)
stopifnot(isTRUE(attr(got_ok, "ok")))
stopifnot(identical(got_ok$value, sum(seq_len(1000L))))

cat("mtfuture ok\n")
