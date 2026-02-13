## Regression tests for experimental background()/wait()/cancel().

stopifnot(exists("background"), exists("then"), exists("wait"), exists("cancel"))

old_threads <- getOption("mtlapply.threads")
on.exit(options(mtlapply.threads = old_threads), add = TRUE)
options(mtlapply.threads = 4L)

## Busy-loop helper to exercise worker CPU (not sleep/wait).
burn_cpu <- function(iterations = 1e6L) {
    i <- 1L
    acc <- 0L
    while (i <= iterations) {
        acc <- acc + (i %% 97L)
        i <- i + 1L
    }
    acc
}

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
stopifnot(inherits(try(wait(list(), timeout = -1), silent = TRUE), "try-error"))
f_slow <- background({ burn_cpu(1e6L); 42L })
stopifnot(is.null(wait(list(f_slow), timeout = 0)))
stopifnot(identical(wait(f_slow)$value, 42L))

## Worker error should be surfaced in the returned future value.
f_err <- background(stop("boom from background"))
got_err <- wait(f_err)
stopifnot(inherits(got_err$value, "error"))
stopifnot(grepl("boom from background", conditionMessage(got_err$value), fixed = TRUE))

## Cancel should mark pending/running jobs as cancelled.
f_cancel <- background({ burn_cpu(3e6L); 99L })
stopifnot(isTRUE(cancel(f_cancel)))
got_cancel <- wait(f_cancel)
stopifnot(isTRUE(attr(got_cancel, "cancelled")))
stopifnot(inherits(got_cancel$value, "mt_cancelled"))

## A successful run after error/cancel should still work.
f_ok <- background(sum(seq_len(1000L)))
got_ok <- wait(f_ok)
stopifnot(isTRUE(attr(got_ok, "ok")))
stopifnot(identical(got_ok$value, sum(seq_len(1000L))))

## then() should chain future values.
f_base <- background(10L)
f_plus <- then(f_base, function(x) x + 5L)
f_mul <- then(f_plus, function(x, k) x * k, 3L)
got_mul <- wait(f_mul)
stopifnot(isTRUE(attr(got_mul, "ok")))
stopifnot(identical(got_mul$value, 45L))

## then() should also work when attached after parent is already resolved.
f_done <- background(2L)
stopifnot(identical(wait(f_done)$value, 2L))
f_late <- then(f_done, function(x) x + 9L)
got_late <- wait(f_late)
stopifnot(isTRUE(attr(got_late, "ok")))
stopifnot(identical(got_late$value, 11L))

## then() should propagate parent errors/cancellation without poisoning workers.
f_parent_err <- background(stop("boom in parent"))
f_child_err <- then(f_parent_err, function(x) x + 1L)
got_child_err <- wait(f_child_err)
stopifnot(inherits(got_child_err$value, "error"))
stopifnot(grepl("boom in parent", conditionMessage(got_child_err$value), fixed = TRUE))

f_parent_cancel <- background({ burn_cpu(3e6L); 123L })
f_child_cancel <- then(f_parent_cancel, function(x) x + 1L)
stopifnot(isTRUE(cancel(f_parent_cancel)))
got_child_cancel <- wait(f_child_cancel)
stopifnot(isTRUE(attr(got_child_cancel, "cancelled")))
stopifnot(inherits(got_child_cancel$value, "mt_cancelled"))

## Continuation errors should be reported on the chained future.
f_cont_err <- then(background(1L), function(x) stop("boom in continuation"))
got_cont_err <- wait(f_cont_err)
stopifnot(inherits(got_cont_err$value, "error"))
stopifnot(grepl("boom in continuation", conditionMessage(got_cont_err$value), fixed = TRUE))

## Cancelling a child continuation should not cancel the parent.
f_parent_keep <- background({ burn_cpu(2e6L); 7L })
f_child_drop <- then(f_parent_keep, function(x) x + 1L)
stopifnot(isTRUE(cancel(f_child_drop)))
got_child_drop <- wait(f_child_drop)
stopifnot(isTRUE(attr(got_child_drop, "cancelled")))
got_parent_keep <- wait(f_parent_keep)
stopifnot(isTRUE(attr(got_parent_keep, "ok")))
stopifnot(identical(got_parent_keep$value, 7L))

## Dropping parent handles and running GC should not cancel active chains.
f_gc <- local({
    f0 <- background(1L)
    f1 <- then(f0, function(x) x + 1L)
    rm(f0)
    gc()
    f2 <- then(f1, function(x) x + 1L)
    rm(f1)
    gc()
    f2
})
got_gc <- wait(f_gc)
stopifnot(isTRUE(attr(got_gc, "ok")))
stopifnot(identical(got_gc$value, 3L))

## Long chains should remain stable.
f_chain <- background(1L)
for (i in 1:50) {
    f_chain <- then(f_chain, function(x) x + 1L)
}
got_chain <- wait(f_chain)
stopifnot(isTRUE(attr(got_chain, "ok")))
stopifnot(identical(got_chain$value, 51L))

## Additional successful run after chained error paths should still work.
f_ok2 <- background(sum(seq_len(100L)))
got_ok2 <- wait(f_ok2)
stopifnot(isTRUE(attr(got_ok2, "ok")))
stopifnot(identical(got_ok2$value, sum(seq_len(100L))))

cat("mtfuture ok\n")
