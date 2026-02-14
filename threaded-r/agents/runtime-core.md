# Runtime Core (Agent Scope)

Focus: bootstrap runtime state, threading lifecycle, integration boundaries.

Key files:

- `src/main/main.c`
- `src/main/context.c`
- `src/main/errors.c`
- `src/main/startup.c`
- `src/unix/*`

Responsibilities:

- Initialize/destroy threadpool exactly once per process.
- Keep main-thread execution path as close to upstream as possible when pool idle.
- Define clear transitions for "threadpool inactive" vs "active job running".
- Ensure unwind safety: error in any worker must not poison subsequent serial evaluation.

Common failure modes to guard:

- recursive error handling after worker failure,
- state leakage across jobs,
- stale pointers after cancellation/teardown.
