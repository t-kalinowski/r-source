## Regression test for experimental mtlapply()
##
## This is intended to catch races in global runtime flags (notably GC enable)
## when evaluating in parallel worker interpreters.

stopifnot(exists("mtlapply"))

# Workload should allocate and also touch global caches (symbols/CHARSXPs).
f <- function(i) {
  s <- paste0("sym-", i, "-", i)
  sym <- as.name(s)
  v <- cos(seq_len(i))
  list(s = s, sym = sym, v = v)
}

x <- mtlapply(1:100, f, threads = 4L)
stopifnot(length(x) == 100L)
stopifnot(isTRUE(all.equal(x[[3]]$v, cos(1:3))))

# The critical regression: a GC after mtlapply() must not wedge the interpreter.
invisible(gc())

cat("mtlapply ok\n")

