# Threadpool + Futures (Agent Scope)

Focus: user-facing concurrency API and queue semantics.

Target APIs (initial):

- `mtlapply(X, FUN, ...)` with `lapply` signature parity.
- thread count controlled by `options(threads = n)`.
- `background(expr)` returns a future-like handle.
- `wait(futures)` waits for next completion (pop-done semantics).
- `cancel(future)` best-effort cancellation before execution.

Execution model:

- persistent shared threadpool,
- workers pull jobs from queue,
- nested jobs use same pool and should not deadlock,
- nested work should take precedence over outer queued work when needed.

Future object guidance:

- R-level structure should expose user-inspectable fields (`expr`, `env`, `value`) plus status,
- native handle should live behind an internal pointer field (not user API contract).
