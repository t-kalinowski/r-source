# Invariants Checklist (Systematic Validation)

Use this as the ongoing truth table while rebuilding features on clean upstream.

## A. Execution + semantics

- [ ] `mtlapply(X, FUN, ...)` preserves result ordering.
- [ ] Closure lexical scoping matches `lapply` behavior.
- [ ] Nested threaded calls do not deadlock.
- [ ] Nested threaded calls resolve closure env/state correctly.

## B. Global mutation policy

- [ ] Worker writes to `globalenv()` are rejected.
- [ ] Worker process-global mutations (e.g. `setwd`) are rejected.
- [ ] Rejections are clean, recoverable, and include normal traceback.
- [ ] Local closure-state mutation remains valid.

## C. Error handling

- [ ] Worker errors propagate to caller exactly once.
- [ ] Worker error does not corrupt subsequent serial evaluation.
- [ ] Repeated worker failures do not poison subinterpreters.
- [ ] Handled errors in worker closures (`tryCatch`) remain correct.

## D. Memory + ownership

- [ ] Each subinterpreter has independent protect stack.
- [ ] Each subinterpreter allocates from interpreter-local heap domain.
- [ ] Returned values transfer/splice safely to caller domain.
- [ ] GC coordination preserves correctness under concurrent activity.

## E. C API + package compatibility

- [ ] Worker `.Call` routines that read/allocate locally succeed.
- [ ] Loaded package routines callable from workers without package changes.
- [ ] Package loading/registration constraints are explicit and tested.
- [ ] Existing binary packages load in threaded build (ABI smoke).

## F. Futures API

- [ ] `background()` enqueues and returns future handle.
- [ ] `wait()` provides next-ready semantics for a set of futures.
- [ ] `cancel()` is deterministic for queued/not-yet-running jobs.
- [ ] Futures and `mtlapply` share one pool without starvation/deadlocks.

## G. Performance non-regression

- [ ] Serial path (pool idle) within configured threshold vs baseline CSV.
- [ ] Threaded scaling improves at `threads=2/4/8` on representative cases.
- [ ] Allocation-heavy workloads tracked separately from compute-heavy workloads.
- [ ] Performance artifact CSV + dashboard refreshed at each checkpoint.
